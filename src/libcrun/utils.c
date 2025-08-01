/*
 * crun - OCI runtime written in C
 *
 * Copyright (C) 2017, 2018, 2019 Giuseppe Scrivano <giuseppe@scrivano.org>
 * crun is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * crun is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with crun.  If not, see <http://www.gnu.org/licenses/>.
 */

#define _GNU_SOURCE
#include <config.h>
#include "utils.h"
#include "ring_buffer.h"
#include <stdarg.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/socket.h>
#include <sys/signalfd.h>
#include <sys/epoll.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <pwd.h>
#include <grp.h>
#include <time.h>
#include <sys/time.h>
#include <sys/xattr.h>
#include <sys/sysmacros.h>
#include <sys/vfs.h>
#include <linux/magic.h>
#include <limits.h>
#include <sys/mman.h>
#ifdef HAVE_LINUX_OPENAT2_H
#  include <linux/openat2.h>
#endif
#if HAVE_STDATOMIC_H
#  include <stdatomic.h>
#else
#  define atomic_long volatile long
#endif

#ifndef CLOSE_RANGE_CLOEXEC
#  define CLOSE_RANGE_CLOEXEC (1U << 2)
#endif
#ifndef RESOLVE_IN_ROOT
#  define RESOLVE_IN_ROOT 0x10
#endif
#ifndef __NR_close_range
#  define __NR_close_range 436
#endif
#ifndef __NR_openat2
#  define __NR_openat2 437
#endif

#define MAX_READLINKS 32

static int
syscall_close_range (unsigned int fd, unsigned int max_fd, unsigned int flags)
{
  return (int) syscall (__NR_close_range, fd, max_fd, flags);
}

static int
syscall_openat2 (int dirfd, const char *path, uint64_t flags, uint64_t mode, uint64_t resolve)
{
  struct openat2_open_how
  {
    uint64_t flags;
    uint64_t mode;
    uint64_t resolve;
  } how = {
    .flags = flags,
    .mode = mode,
    .resolve = resolve,
  };

  return (int) syscall (__NR_openat2, dirfd, path, &how, sizeof (how), 0);
}

int
crun_path_exists (const char *path, libcrun_error_t *err)
{
  int ret = access (path, F_OK);
  if (ret < 0)
    {
      if (errno == ENOENT)
        return 0;
      return crun_make_error (err, errno, "access `%s`", path);
    }
  return 1;
}

int
xasprintf (char **str, const char *fmt, ...)
{
  int ret;
  va_list args_list;

  va_start (args_list, fmt);

  ret = vasprintf (str, fmt, args_list);
  if (UNLIKELY (ret < 0))
    OOM ();

  va_end (args_list);
  return ret;
}

int
write_file_at_with_flags (int dirfd, int flags, mode_t mode, const char *name, const void *data, size_t len, libcrun_error_t *err)
{
  cleanup_close int fd = -1;
  int ret = 0;

  fd = openat (dirfd, name, O_CLOEXEC | O_WRONLY | flags, mode);
  if (UNLIKELY (fd < 0))
    return crun_make_error (err, errno, "open `%s` for writing", name);

  ret = safe_write (fd, name, data, len, err);
  if (UNLIKELY (ret < 0))
    return ret;

  return (len > INT_MAX) ? INT_MAX : (int) len;
}

int
detach_process ()
{
  pid_t pid;
  if (setsid () < 0)
    return -1;
  pid = fork ();
  if (pid < 0)
    return -1;
  if (pid != 0)
    _exit (EXIT_SUCCESS);
  return 0;
}

int
get_file_type_fd (int fd, mode_t *mode)
{
  struct stat st;
  int ret;

#ifdef HAVE_STATX
  struct statx stx = {
    0,
  };

  ret = statx (fd, "", AT_EMPTY_PATH | AT_STATX_DONT_SYNC, STATX_TYPE, &stx);
  if (UNLIKELY (ret < 0))
    {
      if (errno == ENOSYS || errno == EINVAL)
        goto fallback;

      return ret;
    }
  *mode = stx.stx_mode;
  return ret;

fallback:
#endif
  ret = fstat (fd, &st);
  *mode = st.st_mode;
  return ret;
}

int
get_file_type_at (int dirfd, mode_t *mode, bool nofollow, const char *path)
{
  int empty_path = path == NULL ? AT_EMPTY_PATH : 0;
  struct stat st;
  int ret;

#ifdef HAVE_STATX
  struct statx stx = {
    0,
  };

  ret = statx (dirfd, path ?: "", empty_path | (nofollow ? AT_SYMLINK_NOFOLLOW : 0) | AT_STATX_DONT_SYNC, STATX_TYPE, &stx);
  if (UNLIKELY (ret < 0))
    {
      if (errno == ENOSYS || errno == EINVAL)
        goto fallback;

      return ret;
    }
  *mode = stx.stx_mode;
  return ret;

fallback:
#endif
  ret = fstatat (dirfd, path ?: "", &st, empty_path | (nofollow ? AT_SYMLINK_NOFOLLOW : 0));
  *mode = st.st_mode;
  return ret;
}

int
get_file_type (mode_t *mode, bool nofollow, const char *path)
{
  return get_file_type_at (AT_FDCWD, mode, nofollow, path);
}

int
create_file_if_missing_at (int dirfd, const char *file, mode_t mode, libcrun_error_t *err)
{
  cleanup_close int fd_write = openat (dirfd, file, O_CLOEXEC | O_CREAT | O_WRONLY, mode);
  if (fd_write < 0)
    {
      mode_t tmp_mode;
      int ret;

      /* On errors, check if the file already exists.  */
      ret = get_file_type_at (dirfd, &tmp_mode, false, file);
      if (ret == 0 && S_ISREG (tmp_mode))
        return 0;

      return crun_make_error (err, errno, "create file `%s`", file);
    }
  return 0;
}

static int
ensure_directory_internal_at (int dirfd, char *path, size_t len, int mode, libcrun_error_t *err)
{
  char *it = path + len;
  int ret = 0;
  bool parent_created = false;

  for (;;)
    {
      ret = mkdirat (dirfd, path, mode);
      if (ret == 0 || errno == EEXIST)
        return 0;

      int saved_errno = errno;
      if (parent_created || errno != ENOENT)
        {
          libcrun_error_t tmp_err = NULL;

          /* On errors check if the directory already exists.  */
          ret = crun_dir_p (path, false, &tmp_err);
          if (ret > 0)
            return 0;
          if (ret < 0)
            crun_error_release (&tmp_err);

          return crun_make_error (err, saved_errno, "create directory `%s`", path);
        }

      while (it > path && *it != '/')
        {
          it--;
          len--;
        }
      if (it == path)
        return 0;

      *it = '\0';
      ret = ensure_directory_internal_at (dirfd, path, len - 1, mode, err);
      *it = '/';
      if (UNLIKELY (ret < 0))
        return ret;

      parent_created = true;
    }
  return ret;
}

int
crun_ensure_directory_at (int dirfd, const char *path, int mode, bool nofollow, libcrun_error_t *err)
{
  int ret;
  cleanup_free char *tmp = xstrdup (path);
  ret = ensure_directory_internal_at (dirfd, tmp, strlen (tmp), mode, err);
  if (UNLIKELY (ret < 0))
    return ret;

  ret = crun_dir_p_at (dirfd, path, nofollow, err);
  if (UNLIKELY (ret < 0))
    return ret;

  if (ret == 0)
    return crun_make_error (err, ENOTDIR, "the path `%s` is not a directory", path);

  return 0;
}

static int
check_fd_is_path (const char *path, int fd, const char *fdname, libcrun_error_t *err)
{
  proc_fd_path_t fdpath;
  size_t path_len = strlen (path);
  char link[PATH_MAX];
  int ret;

  get_proc_self_fd_path (fdpath, fd);
  ret = TEMP_FAILURE_RETRY (readlink (fdpath, link, sizeof (link)));
  if (UNLIKELY (ret < 0))
    return crun_make_error (err, errno, "readlink `%s`", fdname);

  if (((size_t) ret) != path_len || memcmp (link, path, path_len))
    return crun_make_error (err, 0, "target `%s` does not point to the directory `%s`", fdname, path);

  return 0;
}

static int
check_fd_under_path (const char *rootfs, size_t rootfslen, int fd, const char *fdname, libcrun_error_t *err)
{
  proc_fd_path_t fdpath;
  char link[PATH_MAX];
  int ret;

  get_proc_self_fd_path (fdpath, fd);
  ret = TEMP_FAILURE_RETRY (readlink (fdpath, link, sizeof (link)));
  if (UNLIKELY (ret < 0))
    return crun_make_error (err, errno, "readlink `%s`", fdname);

  if (((size_t) ret) <= rootfslen || memcmp (link, rootfs, rootfslen) != 0 || link[rootfslen] != '/')
    return crun_make_error (err, 0, "target `%s` not under the directory `%s`", fdname, rootfs);

  return 0;
}

/* Check if *oldfd is a valid fd and close it.  Then store newfd into *oldfd.  */
static void
close_and_replace (int *oldfd, int newfd)
{
  if (*oldfd >= 0)
    TEMP_FAILURE_RETRY (close (*oldfd));

  *oldfd = newfd;
}

/* Defined in chroot_realpath.c  */
char *chroot_realpath (const char *chroot, const char *path, char resolved_path[]);

static int
safe_openat_fallback (int dirfd, const char *rootfs, const char *path, int flags,
                      int mode, libcrun_error_t *err)
{
  const char *path_in_chroot;
  cleanup_close int fd = -1;
  char buffer[PATH_MAX];
  size_t rootfs_len = strlen (rootfs);
  int ret;

  path_in_chroot = chroot_realpath (rootfs, path, buffer);
  if (path_in_chroot == NULL)
    return crun_make_error (err, errno, "cannot resolve `%s` under rootfs", path);

  path_in_chroot += rootfs_len;
  path_in_chroot = consume_slashes (path_in_chroot);

  /* If the path is empty we are at the root, dup the dirfd itself.  */
  if (path_in_chroot[0] == '\0')
    {
      ret = dup (dirfd);
      if (UNLIKELY (ret < 0))
        return crun_make_error (err, errno, "dup `%s`", rootfs);
      return ret;
    }

  ret = openat (dirfd, path_in_chroot, flags, mode);
  if (UNLIKELY (ret < 0))
    return crun_make_error (err, errno, "open `%s`", path);

  fd = ret;

  ret = check_fd_under_path (rootfs, rootfs_len, fd, path, err);
  if (UNLIKELY (ret < 0))
    return ret;

  ret = fd;
  fd = -1;
  return ret;
}

int
safe_openat (int dirfd, const char *rootfs, const char *path, int flags, int mode,
             libcrun_error_t *err)
{
  static bool openat2_supported = true;
  int ret;

  if (is_empty_string (path))
    {
      cleanup_close int fd = -1;

      fd = open (rootfs, flags, mode);
      if (UNLIKELY (fd < 0))
        return crun_make_error (err, errno, "open `%s`", rootfs);

      ret = check_fd_is_path (rootfs, fd, path, err);
      if (UNLIKELY (ret < 0))
        return ret;

      ret = fd;
      fd = -1;
      return ret;
    }

  if (openat2_supported)
    {
    repeat:
      ret = syscall_openat2 (dirfd, path, flags, mode, RESOLVE_IN_ROOT);
      if (UNLIKELY (ret < 0))
        {
          if (errno == EINTR || errno == EAGAIN)
            goto repeat;
          if (errno == ENOSYS)
            openat2_supported = false;
          if (errno == ENOSYS || errno == EINVAL || errno == EPERM)
            return safe_openat_fallback (dirfd, rootfs, path, flags, mode, err);

          return crun_make_error (err, errno, "openat2 `%s`", path);
        }

      return ret;
    }

  return safe_openat_fallback (dirfd, rootfs, path, flags, mode, err);
}

ssize_t
safe_readlinkat (int dfd, const char *name, char **buffer, ssize_t hint, libcrun_error_t *err)
{
  /* Add 1 to make room for the NUL terminator.  */
  ssize_t buf_size = hint > 0 ? hint + 1 : 512;
  cleanup_free char *tmp_buf = NULL;
  ssize_t size;

  do
    {
      if (tmp_buf != NULL)
        buf_size += 256;

      tmp_buf = xrealloc (tmp_buf, buf_size);

      size = readlinkat (dfd, name, tmp_buf, buf_size);
      if (UNLIKELY (size < 0))
        return crun_make_error (err, errno, "readlink `%s`", name);
  } while (size == buf_size);

  /* Always NUL terminate the buffer.  */
  tmp_buf[size] = '\0';

  /* Move ownership to BUFFER.  */
  *buffer = tmp_buf;
  tmp_buf = NULL;

  return size;
}

static int
crun_safe_ensure_at (bool do_open, bool dir, int dirfd, const char *dirpath,
                     const char *path, int mode, int max_readlinks, libcrun_error_t *err)
{
  cleanup_close int wd_cleanup = -1;
  cleanup_free char *npath = NULL;
  bool last_component = false;
  size_t depth = 0;
  const char *cur;
  char *it;
  int cwd;
  int ret;

  if (max_readlinks <= 0)
    return crun_make_error (err, ELOOP, "resolve path `%s`", path);

  path = consume_slashes (path);

  /* Empty path, nothing to do.  */
  if (*path == '\0')
    {
      if (do_open)
        return open (dirpath, O_CLOEXEC | O_PATH, 0);
      return 0;
    }

  npath = xstrdup (path);

  cwd = dirfd;
  cur = npath;
  it = strchr (npath, '/');
  while (cur)
    {
      if (it)
        *it = '\0';
      else
        last_component = true;

      if (cur[0] == '\0')
        break;

      if (strcmp (cur, ".") == 0)
        goto next;
      else if (strcmp (cur, ".."))
        depth++;
      else
        {
          if (depth)
            depth--;
          else
            {
              /* Start from the root.  */
              close_and_reset (&wd_cleanup);
              cwd = dirfd;
              goto next;
            }
        }

      if (last_component && ! dir)
        {
          ret = openat (cwd, cur, O_CLOEXEC | O_CREAT | O_WRONLY | O_NOFOLLOW, 0700);
          if (UNLIKELY (ret < 0))
            {
              /* If the last component is a symlink, repeat the lookup with the resolved path.  */
              if (errno == ELOOP)
                {
                  cleanup_free char *resolved_path = NULL;

                  ret = safe_readlinkat (cwd, cur, &resolved_path, 0, err);
                  if (LIKELY (ret >= 0))
                    {
                      return crun_safe_ensure_at (do_open, dir, dirfd,
                                                  dirpath,
                                                  resolved_path, mode,
                                                  max_readlinks - 1, err);
                    }
                  crun_error_release (err);
                }
              /* If the previous openat fails, attempt to open the file in O_PATH mode.  */
              ret = openat (cwd, cur, O_CLOEXEC | O_PATH, 0);
              if (ret < 0)
                return crun_make_error (err, errno, "open `%s/%s`", dirpath, npath);
            }

          if (do_open)
            return ret;

          close_and_replace (&wd_cleanup, ret);
          return 0;
        }

      ret = mkdirat (cwd, cur, mode);
      if (ret < 0)
        {
          if (errno != EEXIST)
            return crun_make_error (err, errno, "mkdir `/%s`", npath);
        }

      cwd = safe_openat (dirfd, dirpath, npath, (last_component ? O_PATH : 0) | O_CLOEXEC, 0, err);
      if (UNLIKELY (cwd < 0))
        return crun_error_wrap (err, "creating `/%s`", path);

      if (! last_component)
        {
          mode_t st_mode;

          ret = get_file_type_at (cwd, &st_mode, true, NULL);
          if (UNLIKELY (ret < 0))
            {
              int saved_errno = errno;

              close (cwd);
              return crun_make_error (err, saved_errno, "stat `%s`", npath);
            }
          if ((st_mode & S_IFMT) != S_IFDIR)
            {
              close (cwd);
              return crun_make_error (err, ENOTDIR, "error creating directory `%s` since `%s` exists and it is not a directory", path, npath);
            }
        }

      close_and_replace (&wd_cleanup, cwd);

    next:
      if (it == NULL)
        break;

      cur = consume_slashes (it + 1);
      *it = '/';
      it = strchr (cur, '/');
    }

  if (do_open)
    {
      if (cwd == dirfd)
        {
          ret = dup (dirfd);
          if (UNLIKELY (ret < 0))
            return crun_make_error (err, errno, "dup `%s`", dirpath);
          return ret;
        }

      wd_cleanup = -1;
      return cwd;
    }

  return 0;
}

int
crun_safe_create_and_open_ref_at (bool dir, int dirfd, const char *dirpath, const char *path, int mode, libcrun_error_t *err)
{
  int ret;

  /* If the file/dir already exists, just open it.  */
  ret = safe_openat (dirfd, dirpath, path, O_PATH | O_CLOEXEC, 0, err);
  if (LIKELY (ret >= 0))
    return ret;

  crun_error_release (err);
  return crun_safe_ensure_at (true, dir, dirfd, dirpath, path, mode, MAX_READLINKS, err);
}

int
crun_safe_ensure_directory_at (int dirfd, const char *dirpath, const char *path, int mode,
                               libcrun_error_t *err)
{
  return crun_safe_ensure_at (false, true, dirfd, dirpath, path, mode, MAX_READLINKS, err);
}

int
crun_safe_ensure_file_at (int dirfd, const char *dirpath, const char *path, int mode,
                          libcrun_error_t *err)
{
  return crun_safe_ensure_at (false, false, dirfd, dirpath, path, mode, MAX_READLINKS, err);
}

int
crun_ensure_directory (const char *path, int mode, bool nofollow, libcrun_error_t *err)
{
  return crun_ensure_directory_at (AT_FDCWD, path, mode, nofollow, err);
}

static int
get_file_size (int fd, off_t *size)
{
  struct stat st;
  int ret;
#ifdef HAVE_STATX
  struct statx stx = {
    0,
  };

  ret = statx (fd, "", AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW | AT_STATX_DONT_SYNC, STATX_SIZE, &stx);
  if (UNLIKELY (ret < 0))
    {
      if (errno == ENOSYS || errno == EINVAL)
        goto fallback;
      return ret;
    }
  *size = stx.stx_size;

  return ret;

fallback:
#endif
  ret = fstat (fd, &st);
  *size = st.st_size;
  return ret;
}

int
crun_dir_p_at (int dirfd, const char *path, bool nofollow, libcrun_error_t *err)
{
  mode_t mode;
  int ret;

  ret = get_file_type_at (dirfd, &mode, nofollow, path);
  if (UNLIKELY (ret < 0))
    return crun_make_error (err, errno, "stat `%s`", path);

  return S_ISDIR (mode) ? 1 : 0;
}

int
crun_dir_p (const char *path, bool nofollow, libcrun_error_t *err)
{
  return crun_dir_p_at (AT_FDCWD, path, nofollow, err);
}

int
check_running_in_user_namespace (libcrun_error_t *err)
{
  cleanup_free char *buffer = NULL;
  static int run_in_userns = -1;
  size_t len;
  int ret;

  ret = run_in_userns;
  if (ret >= 0)
    return ret;

  ret = read_all_file ("/proc/self/uid_map", &buffer, &len, err);
  if (UNLIKELY (ret < 0))
    {
      /* If the file does not exist, then the kernel does not support user namespaces and we for sure aren't in one.  */
      if (crun_error_get_errno (err) == ENOENT)
        {
          crun_error_release (err);
          run_in_userns = 0;
          return run_in_userns;
        }
      return ret;
    }

  ret = strstr (buffer, "4294967295") ? 0 : 1;
  run_in_userns = ret;
  return ret;
}

static size_t
get_page_size ()
{
  static atomic_long cached_pagesize = 0;
  atomic_long pagesize = cached_pagesize;
  if (pagesize == 0)
    {
      pagesize = (long) sysconf (_SC_PAGESIZE);
      cached_pagesize = pagesize;
    }
  return (size_t) pagesize;
}

static int selinux_enabled = -1;
static int apparmor_enabled = -1;

int
libcrun_initialize_selinux (libcrun_error_t *err)
{
  cleanup_free char *out = NULL;
  cleanup_close int fd = -1;
  size_t len;
  int ret;

  if (selinux_enabled >= 0)
    return selinux_enabled;

  fd = open ("/proc/mounts", O_RDONLY | O_CLOEXEC);
  if (UNLIKELY (fd < 0))
    return crun_make_error (err, errno, "open `/proc/mounts`");

  ret = read_all_fd_with_size_hint (fd, "/proc/mounts", &out, &len, get_page_size (), err);
  if (UNLIKELY (ret < 0))
    return ret;

  selinux_enabled = strstr (out, "selinux") ? 1 : 0;

  return selinux_enabled;
}

int
libcrun_initialize_apparmor (libcrun_error_t *err)
{
  cleanup_close int fd = -1;
  int size;
  char buf[2];

  if (apparmor_enabled >= 0)
    return apparmor_enabled;

  if (crun_dir_p_at (AT_FDCWD, "/sys/kernel/security/apparmor", true, err))
    {
      fd = open ("/sys/module/apparmor/parameters/enabled", O_RDONLY | O_CLOEXEC);
      if (fd == -1)
        return 0;

      size = TEMP_FAILURE_RETRY (read (fd, &buf, 2));

      apparmor_enabled = size > 0 && buf[0] == 'Y' ? 1 : 0;
    }

  return apparmor_enabled;
}

static int
libcrun_is_selinux_enabled (libcrun_error_t *err)
{
  if (selinux_enabled < 0)
    return crun_make_error (err, 0, "SELinux is not initialized correctly");
  return selinux_enabled;
}

int
add_selinux_mount_label (char **retlabel, const char *data, const char *label, const char *context_type, libcrun_error_t *err)
{
  int ret;

  ret = libcrun_is_selinux_enabled (err);
  if (UNLIKELY (ret < 0))
    return ret;

  if (label && ret)
    {
      if (data && *data)
        xasprintf (retlabel, "%s,%s=\"%s\"", data, context_type, label);
      else
        xasprintf (retlabel, "%s=\"%s\"", context_type, label);
      return 0;
    }
  *retlabel = xstrdup (data);
  return 0;
}

static const char *
lsm_attr_path (const char *lsm, const char *fname, libcrun_error_t *err)
{
  cleanup_close int attr_dirfd = -1;
  cleanup_close int lsm_dirfd = -1;
  char *attr_path = NULL;

  attr_dirfd = open ("/proc/thread-self/attr", O_DIRECTORY | O_PATH | O_CLOEXEC);
  if (UNLIKELY (attr_dirfd < 0))
    {
      crun_make_error (err, errno, "open `/proc/thread-self/attr`");
      return NULL;
    }

  // Check for newer scoped interface in /proc/thread-self/attr/<lsm>
  if (lsm != NULL)
    {
      lsm_dirfd = openat (attr_dirfd, lsm, O_DIRECTORY | O_PATH | O_CLOEXEC);

      if (UNLIKELY (lsm_dirfd < 0 && errno != ENOENT))
        {
          crun_make_error (err, errno, "open `/proc/thread-self/attr/%s`", lsm);
          return NULL;
        }
    }
  // Use scoped interface if available, fall back to unscoped
  xasprintf (&attr_path, "/proc/thread-self/attr/%s%s%s", lsm_dirfd >= 0 ? lsm : "", lsm_dirfd >= 0 ? "/" : "", fname);

  return attr_path;
}

static int
check_proc_super_magic (int fd, const char *path, libcrun_error_t *err)
{
  struct statfs sfs;

  int ret = fstatfs (fd, &sfs);
  if (UNLIKELY (ret < 0))
    return crun_make_error (err, errno, "statfs `%s`", path);

  if (sfs.f_type != PROC_SUPER_MAGIC)
    return crun_make_error (err, 0, "the file `%s` is not on a `procfs` file system", path);

  return 0;
}

static int
set_security_attr (const char *lsm, const char *fname, const char *data, libcrun_error_t *err)
{
  int ret;

  cleanup_free const char *attr_path = lsm_attr_path (lsm, fname, err);
  cleanup_close int fd = -1;

  if (UNLIKELY (attr_path == NULL))
    return -1;

  fd = open (attr_path, O_WRONLY | O_CLOEXEC);

  if (UNLIKELY (fd < 0))
    return crun_make_error (err, errno, "open `%s`", attr_path);

  ret = check_proc_super_magic (fd, attr_path, err);
  if (UNLIKELY (ret < 0))
    return ret;

  // Write out data
  ret = TEMP_FAILURE_RETRY (write (fd, data, strlen (data)));
  if (UNLIKELY (ret < 0))
    return crun_make_error (err, errno, "write to file `%s`", attr_path);

  return 0;
}

int
set_selinux_label (const char *label, bool now, libcrun_error_t *err)
{
  int ret;

  ret = libcrun_is_selinux_enabled (err);
  if (UNLIKELY (ret < 0))
    return ret;

  if (ret)
    return set_security_attr (NULL, now ? "current" : "exec", label, err);
  return 0;
}

static int
libcrun_is_apparmor_enabled (libcrun_error_t *err)
{
  if (apparmor_enabled < 0)
    return crun_make_error (err, 0, "AppArmor is not initialized correctly");
  return apparmor_enabled;
}

static int
is_current_process_confined (libcrun_error_t *err)
{
  cleanup_free const char *attr_path = lsm_attr_path ("apparmor", "current", err);
  cleanup_close int fd = -1;
  char buf[256];

  if (UNLIKELY (attr_path == NULL))
    return -1;

  fd = open (attr_path, O_RDONLY | O_CLOEXEC);

  if (UNLIKELY (fd < 0))
    return crun_make_error (err, errno, "open `%s`", attr_path);

  if (UNLIKELY (check_proc_super_magic (fd, attr_path, err)))
    return -1;

  ssize_t bytes_read = read (fd, buf, sizeof (buf) - 1);
  if (UNLIKELY (bytes_read < 0))
    return crun_make_error (err, errno, "read from `%s`", attr_path);

#define UNCONFINED "unconfined"
#define UNCONFINED_LEN (ssize_t) (sizeof (UNCONFINED) - 1)
  return bytes_read >= UNCONFINED_LEN && memcmp (buf, UNCONFINED, UNCONFINED_LEN);
}

int
set_apparmor_profile (const char *profile, bool no_new_privileges, bool now, libcrun_error_t *err)
{
  int ret;

  ret = libcrun_is_apparmor_enabled (err);
  if (UNLIKELY (ret < 0))
    return ret;

  if (ret)
    {
      cleanup_free char *buf = NULL;
      ret = is_current_process_confined (err);
      if (UNLIKELY (ret < 0))
        return ret;
      // if confined only way for apparmor to allow change of profile with NNP is with stacking
      xasprintf (&buf, "%s %s", no_new_privileges && ret ? "stack" : now ? "changeprofile"
                                                                         : "exec",
                 profile);

      return set_security_attr ("apparmor", now ? "current" : "exec", buf, err);
    }
  return 0;
}

int
read_all_fd_with_size_hint (int fd, const char *description, char **out, size_t *len, size_t size_hint, libcrun_error_t *err)
{
  cleanup_free char *buf = NULL;
  size_t nread, allocated;
  size_t pagesize = 0;
  off_t size = 0;
  int ret;

  if (size_hint)
    allocated = size_hint;
  else
    {
      ret = get_file_size (fd, &size);
      if (UNLIKELY (ret < 0))
        return crun_make_error (err, errno, "stat `%s`", description);

      allocated = size == 0 ? 1023 : size;
    }

  /* NUL terminate the buffer.  */
  buf = xmalloc (allocated + 1);
  nread = 0;
  while ((size && nread < (size_t) size) || size == 0)
    {
      ret = TEMP_FAILURE_RETRY (read (fd, buf + nread, allocated - nread));
      if (UNLIKELY (ret < 0))
        return crun_make_error (err, errno, "read from file `%s`", description);

      if (ret == 0)
        break;

      nread += ret;

      if (nread == allocated)
        {
          if (size)
            break;

          if (pagesize == 0)
            pagesize = get_page_size ();

          allocated += pagesize;

          buf = xrealloc (buf, allocated + 1);
        }
    }
  if (nread + 1 < allocated)
    {
      /* shrink the buffer to the used size if it was allocated a bigger block.  */
      char *tmp = realloc (buf, nread + 1);
      if (tmp)
        buf = tmp;
    }

  buf[nread] = '\0';
  *out = buf;
  buf = NULL;
  if (len)
    *len = nread;
  return 0;
}

int
read_all_file_at (int dirfd, const char *path, char **out, size_t *len, libcrun_error_t *err)
{
  cleanup_close int fd = -1;

  fd = TEMP_FAILURE_RETRY (openat (dirfd, path, O_RDONLY | O_CLOEXEC));
  if (UNLIKELY (fd < 0))
    return crun_make_error (err, errno, "open `%s`", path);

  return read_all_fd (fd, path, out, len, err);
}

int
read_all_file (const char *path, char **out, size_t *len, libcrun_error_t *err)
{
  if (strcmp (path, "-") == 0)
    path = "/dev/stdin";

  return read_all_file_at (AT_FDCWD, path, out, len, err);
}

int
get_realpath_to_file (int dirfd, const char *path_name, char **absolute_path, libcrun_error_t *err)
{
  cleanup_close int targetfd = -1;

  targetfd = TEMP_FAILURE_RETRY (openat (dirfd, path_name, O_RDONLY | O_CLOEXEC));
  if (UNLIKELY (targetfd < 0))
    return crun_make_error (err, errno, "open `%s`", path_name);
  else
    {
      ssize_t len;
      proc_fd_path_t target_fd_path;

      get_proc_self_fd_path (target_fd_path, targetfd);
      len = safe_readlinkat (AT_FDCWD, target_fd_path, absolute_path, 0, err);
      if (UNLIKELY (len < 0))
        {
          crun_error_release (err);
          return crun_make_error (err, errno, "error unable to provide absolute path to file `%s`", path_name);
        }
    }

  return 0;
}

int
open_unix_domain_client_socket (const char *path, int dgram, libcrun_error_t *err)
{
  struct sockaddr_un addr = {};
  int ret;
  proc_fd_path_t name_buf;
  cleanup_close int destfd = -1;
  cleanup_close int fd = -1;

  libcrun_debug ("Opening UNIX domain socket: %s", path);

  fd = socket (AF_UNIX, dgram ? SOCK_DGRAM : SOCK_STREAM, 0);
  if (UNLIKELY (fd < 0))
    return crun_make_error (err, errno, "create UNIX socket");

  if (strlen (path) >= sizeof (addr.sun_path))
    {
      destfd = open (path, O_PATH | O_CLOEXEC);
      if (UNLIKELY (destfd < 0))
        return crun_make_error (err, errno, "open `%s`", path);

      get_proc_self_fd_path (name_buf, destfd);

      path = name_buf;
    }

  strcpy (addr.sun_path, path);
  addr.sun_family = AF_UNIX;
  ret = connect (fd, (struct sockaddr *) &addr, sizeof (addr));
  if (UNLIKELY (ret < 0))
    return crun_make_error (err, errno, "connect socket to `%s`", path);

  ret = fd;
  fd = -1;

  return ret;
}

int
open_unix_domain_socket (const char *path, int dgram, libcrun_error_t *err)
{
  struct sockaddr_un addr = {};
  proc_fd_path_t name_buf;
  int ret;
  cleanup_close int fd = socket (AF_UNIX, dgram ? SOCK_DGRAM : SOCK_STREAM, 0);
  if (UNLIKELY (fd < 0))
    return crun_make_error (err, errno, "create UNIX socket");

  if (strlen (path) >= sizeof (addr.sun_path))
    {
      get_proc_self_fd_path (name_buf, fd);
      path = name_buf;
    }
  strcpy (addr.sun_path, path);
  addr.sun_family = AF_UNIX;
  ret = bind (fd, (struct sockaddr *) &addr, sizeof (addr));
  if (UNLIKELY (ret < 0))
    return crun_make_error (err, errno, "bind socket to `%s`", path);

  if (! dgram)
    {
      ret = listen (fd, 1);
      if (UNLIKELY (ret < 0))
        return crun_make_error (err, errno, "listen on socket");
    }

  ret = fd;
  fd = -1;

  return ret;
}

int
send_fd_to_socket (int server, int fd, libcrun_error_t *err)
{
  return send_fd_to_socket_with_payload (server, fd, NULL, 0, err);
}

int
send_fd_to_socket_with_payload (int server, int fd, const char *payload, size_t payload_len, libcrun_error_t *err)
{
  int ret;
  struct cmsghdr *cmsg = NULL;
  struct iovec iov[2];
  struct msghdr msg = {};
  char ctrl_buf[CMSG_SPACE (1 + sizeof (int))] = {};
  char data[1];

  data[0] = ' ';
  iov[0].iov_base = data;
  iov[0].iov_len = sizeof (data);

  if (payload_len > 0)
    {
      iov[0].iov_base = (void *) payload;
      iov[0].iov_len = payload_len;
    }

  msg.msg_name = NULL;
  msg.msg_namelen = 0;
  msg.msg_iov = iov;
  msg.msg_iovlen = 1;
  msg.msg_controllen = CMSG_SPACE (sizeof (int));
  msg.msg_control = ctrl_buf;

  cmsg = CMSG_FIRSTHDR (&msg);
  cmsg->cmsg_level = SOL_SOCKET;
  cmsg->cmsg_type = SCM_RIGHTS;
  cmsg->cmsg_len = CMSG_LEN (sizeof (int));

  *((int *) CMSG_DATA (cmsg)) = fd;

  ret = TEMP_FAILURE_RETRY (sendmsg (server, &msg, 0));
  if (UNLIKELY (ret < 0))
    return crun_make_error (err, errno, "sendmsg");
  return 0;
}

int
receive_fd_from_socket_with_payload (int from, char *payload, size_t payload_len, libcrun_error_t *err)
{
  cleanup_close int fd = -1;
  int ret;
  struct iovec iov[1];
  struct msghdr msg = {};
  char ctrl_buf[CMSG_SPACE (sizeof (int))] = {};
  char data[1];
  struct cmsghdr *cmsg;

  data[0] = ' ';
  iov[0].iov_base = data;
  iov[0].iov_len = sizeof (data);

  if (payload_len > 0)
    {
      iov[0].iov_base = (void *) payload;
      iov[0].iov_len = payload_len;
    }

  msg.msg_name = NULL;
  msg.msg_namelen = 0;
  msg.msg_iov = iov;
  msg.msg_iovlen = 1;
  msg.msg_controllen = CMSG_SPACE (sizeof (int));
  msg.msg_control = ctrl_buf;

  ret = TEMP_FAILURE_RETRY (recvmsg (from, &msg, 0));
  if (UNLIKELY (ret < 0))
    return crun_make_error (err, errno, "recvmsg");
  if (UNLIKELY (ret == 0))
    return crun_make_error (err, 0, "read FD: connection closed");

  cmsg = CMSG_FIRSTHDR (&msg);
  if (cmsg == NULL)
    return crun_make_error (err, 0, "no msg received");
  memcpy (&fd, CMSG_DATA (cmsg), sizeof (fd));

  ret = fd;
  fd = -1;
  return ret;
}

int
receive_fd_from_socket (int from, libcrun_error_t *err)
{
  return receive_fd_from_socket_with_payload (from, NULL, 0, err);
}

int
create_socket_pair (int *pair, libcrun_error_t *err)
{
  int ret = socketpair (AF_UNIX, SOCK_SEQPACKET, 0, pair);
  if (UNLIKELY (ret < 0))
    return crun_make_error (err, errno, "socketpair");
  return 0;
}

int
create_signalfd (sigset_t *mask, libcrun_error_t *err)
{
  int ret = signalfd (-1, mask, 0);
  if (UNLIKELY (ret < 0))
    return crun_make_error (err, errno, "signalfd");
  return ret;
}

static int
epoll_helper_toggle (int epollfd, int fd, int events, libcrun_error_t *err)
{
  struct epoll_event ev = {};
  bool add = events != 0;
  int ret;

  ev.events = events;
  ev.data.fd = fd;
  ret = epoll_ctl (epollfd, add ? EPOLL_CTL_ADD : EPOLL_CTL_DEL, fd, &ev);
  if (UNLIKELY (ret < 0))
    {
      if (errno == EEXIST || errno == ENOENT)
        return 0;
      return crun_make_error (err, errno, "epoll_ctl `%s` `%d`", add ? "add" : "del", fd);
    }
  return 0;
}

int
epoll_helper (int *in_fds, int *in_levelfds, int *out_fds, int *out_levelfds, libcrun_error_t *err)
{
  struct epoll_event ev;
  cleanup_close int epollfd = -1;
  int ret;

  int *it;
  epollfd = epoll_create1 (0);
  if (UNLIKELY (epollfd < 0))
    return crun_make_error (err, errno, "epoll_create1");

#define ADD_FDS(FDS, EVENTS)                                            \
  for (it = FDS; *it >= 0; it++)                                        \
    {                                                                   \
      ev.events = EVENTS;                                               \
      ev.data.fd = *it;                                                 \
      ret = epoll_ctl (epollfd, EPOLL_CTL_ADD, *it, &ev);               \
      if (UNLIKELY (ret < 0))                                           \
        return crun_make_error (err, errno, "epoll_ctl add `%d`", *it); \
    }

  if (in_fds)
    ADD_FDS (in_fds, EPOLLIN);
  if (in_levelfds)
    ADD_FDS (in_levelfds, EPOLLIN | EPOLLET);
  if (out_fds)
    ADD_FDS (out_fds, EPOLLOUT);
  if (out_levelfds)
    ADD_FDS (out_levelfds, EPOLLOUT | EPOLLET);

  ret = epollfd;
  epollfd = -1;
  return ret;
}

int
copy_from_fd_to_fd (int src, int dst, int consume, libcrun_error_t *err)
{
  int ret;
  ssize_t nread;
  size_t pagesize = get_page_size ();
#ifdef HAVE_COPY_FILE_RANGE
  bool can_copy_file_range = true;
#endif
  do
    {
      cleanup_free char *buffer = NULL;
      ssize_t remaining;

#ifdef HAVE_COPY_FILE_RANGE
      if (can_copy_file_range)
        {
          nread = copy_file_range (src, NULL, dst, NULL, pagesize, 0);
          if (nread < 0 && (errno == EINVAL || errno == EXDEV))
            {
              can_copy_file_range = false;
              goto fallback;
            }
          if (consume && nread < 0 && errno == EAGAIN)
            return 0;
          if (nread < 0 && errno == EIO)
            return 0;
          if (UNLIKELY (nread < 0))
            return crun_make_error (err, errno, "copy_file_range");
          continue;
        }
    fallback:
#endif

      buffer = xmalloc (pagesize);
      nread = TEMP_FAILURE_RETRY (read (src, buffer, pagesize));
      if (consume && nread < 0 && errno == EAGAIN)
        return 0;
      if (nread < 0 && errno == EIO)
        return 0;
      if (UNLIKELY (nread < 0))
        return crun_make_error (err, errno, "read");

      remaining = nread;
      while (remaining)
        {
          ret = TEMP_FAILURE_RETRY (write (dst, buffer + nread - remaining, remaining));
          if (UNLIKELY (ret < 0))
            return crun_make_error (err, errno, "write");
          remaining -= ret;
        }
  } while (consume && nread);

  return 0;
}

int
run_process (char **args, libcrun_error_t *err)
{
  pid_t pid = fork ();
  if (UNLIKELY (pid < 0))
    return crun_make_error (err, errno, "fork");
  if (pid)
    {
      int r, status;
      r = TEMP_FAILURE_RETRY (waitpid (pid, &status, 0));
      if (r < 0)
        return crun_make_error (err, errno, "waitpid");
      if (WIFEXITED (status) || WIFSIGNALED (status))
        return WEXITSTATUS (status);
    }

  execvp (args[0], args);
  _exit (EXIT_FAILURE);
}

#ifndef HAVE_FGETPWENT_R
static unsigned
atou (char **s)
{
  unsigned x;
  for (x = 0; **s - '0' < 10; ++*s)
    x = 10 * x + (**s - '0');
  return x;
}

int
fgetpwent_r (FILE *f, struct passwd *pw, char *line, size_t size, struct passwd **res)
{
  char *s;
  int rv = 0;
  for (;;)
    {
      line[size - 1] = '\xff';
      if ((fgets (line, size, f) == NULL) || ferror (f) || line[size - 1] != '\xff')
        {
          rv = (line[size - 1] != '\xff') ? ERANGE : ENOENT;
          line = 0;
          pw = 0;
          break;
        }
      line[strcspn (line, "\n")] = 0;

      s = line;
      pw->pw_name = s++;
      if (! (s = strchr (s, ':')))
        continue;

      *s++ = 0;
      pw->pw_passwd = s;
      if (! (s = strchr (s, ':')))
        continue;

      *s++ = 0;
      pw->pw_uid = atou (&s);
      if (*s != ':')
        continue;

      *s++ = 0;
      pw->pw_gid = atou (&s);
      if (*s != ':')
        continue;

      *s++ = 0;
      pw->pw_gecos = s;
      if (! (s = strchr (s, ':')))
        continue;

      *s++ = 0;
      pw->pw_dir = s;
      if (! (s = strchr (s, ':')))
        continue;

      *s++ = 0;
      pw->pw_shell = s;
      break;
    }
  *res = pw;
  if (rv)
    errno = rv;
  return rv;
}
#endif

int
set_home_env (uid_t id)
{
  struct passwd pwd;
  cleanup_free char *buf = NULL;
  long buf_size;
  cleanup_file FILE *stream = NULL;
  int ret = -1;

  buf_size = sysconf (_SC_GETPW_R_SIZE_MAX);
  if (buf_size < 0)
    buf_size = 1024;

  buf = xmalloc (buf_size);

  stream = fopen ("/etc/passwd", "re");
  if (stream == NULL)
    goto error;

  for (;;)
    {
      struct passwd *ret_pw = NULL;

      ret = fgetpwent_r (stream, &pwd, buf, buf_size, &ret_pw);
      if (UNLIKELY (ret != 0))
        {
          if (errno != ERANGE)
            goto error;

          buf_size *= 2;
          buf = xrealloc (buf, buf_size);
          continue;
        }

      if (ret_pw && ret_pw->pw_uid == id)
        {
          setenv ("HOME", ret_pw->pw_dir, 1);
          return 0;
        }
    }

error:
  /* Let callers handle the error if the user was not found. */
  return ret ? -errno : 0;
}

/*if subuid or subgid exist, take the first range for the user */
static int
getsubidrange (uid_t id, int is_uid, uint32_t *from, uint32_t *len)
{
  cleanup_file FILE *input = NULL;
  cleanup_free char *lineptr = NULL;
  size_t lenlineptr = 0, len_name;
  long buf_size;
  cleanup_free char *buf = NULL;
  const char *name;
  struct passwd pwd;

  buf_size = sysconf (_SC_GETPW_R_SIZE_MAX);
  if (buf_size < 0)
    buf_size = 1024;

  buf = xmalloc (buf_size);
  for (;;)
    {
      int ret;
      struct passwd *ret_pw = NULL;

      ret = getpwuid_r (id, &pwd, buf, buf_size, &ret_pw);
      if (LIKELY (ret == 0))
        {
          if (ret_pw)
            {
              name = ret_pw->pw_name;
              break;
            }
          return -1;
        }

      if (ret != ERANGE)
        return -1;

      buf_size *= 2;
      buf = xrealloc (buf, buf_size);
    }

  len_name = strlen (name);

  input = fopen (is_uid ? "/etc/subuid" : "/etc/subgid", "re");
  if (input == NULL)
    return -1;

  for (;;)
    {
      char *endptr;
      ssize_t read = getline (&lineptr, &lenlineptr, input);
      if (read < 0)
        return -1;

      if (read < (ssize_t) (len_name + 2))
        continue;

      if (memcmp (lineptr, name, len_name) || lineptr[len_name] != ':')
        continue;

      *from = strtoull (&lineptr[len_name + 1], &endptr, 10);

      if (endptr >= &lineptr[read])
        return -1;

      *len = strtoull (&endptr[1], &endptr, 10);

      return 0;
    }
}

#define MIN(x, y) ((x) < (y) ? (x) : (y))

int
format_default_id_mapping (char **out, uid_t container_id, uid_t host_uid, uid_t host_id, int is_uid, libcrun_error_t *err)
{
  uint32_t from = 0, available = 0;
  cleanup_free char *buffer = NULL;
  int written = 0;
  int ret, remaining;

  *out = NULL;

  if (getsubidrange (host_uid, is_uid, &from, &available) < 0)
    return 0;

  /* More than enough space for all the mappings.  */
  remaining = 15 * 5 * 3;
  buffer = xmalloc (remaining + 1);

  if (container_id > 0)
    {
      uint32_t used = MIN (container_id, available);
      ret = snprintf (buffer + written, remaining, "%d %d %d\n", 0, from, used);
      if (UNLIKELY (ret >= remaining))
        return crun_make_error (err, 0, "internal error: allocated buffer too small");

      written += written;
      remaining -= ret;

      from += used;
      available -= used;
    }

  /* Host ID -> Container ID.  */
  ret = snprintf (buffer + written, remaining, "%d %d 1\n", container_id, host_id);
  if (UNLIKELY (ret >= remaining))
    return crun_make_error (err, 0, "internal error: allocated buffer too small");
  written += ret;
  remaining -= ret;

  /* Last mapping: use any id that is left.  */
  if (available)
    {
      ret = snprintf (buffer + written, remaining, "%d %d %d\n", container_id + 1, from, available);
      if (UNLIKELY (ret >= remaining))
        return crun_make_error (err, 0, "internal error: allocated buffer too small");
      written += ret;
      remaining -= ret;
    }

  *out = buffer;
  buffer = NULL;
  return written;
}

static int
unset_cloexec_flag (int fd)
{
  int flags = fcntl (fd, F_GETFD);
  if (flags == -1)
    return -1;

  flags &= ~FD_CLOEXEC;

  return fcntl (fd, F_SETFD, flags);
}

static void __attribute__ ((__noreturn__))
run_process_child (char *path, char **args, const char *cwd, char **envp, int pipe_r,
                   int pipe_w, int out_fd, int err_fd)
{
  char *tmp_args[] = { path, NULL };
  libcrun_error_t err = NULL;
  int dev_null_fd = -1;
  int ret;

  ret = mark_or_close_fds_ge_than (3, false, &err);
  if (UNLIKELY (ret < 0))
    libcrun_fail_with_error ((err)->status, "%s", (err)->msg);

  if (out_fd < 0 || err_fd < 0)
    {
      dev_null_fd = open ("/dev/null", O_WRONLY | O_CLOEXEC);
      if (UNLIKELY (dev_null_fd < 0))
        _exit (EXIT_FAILURE);
    }

  TEMP_FAILURE_RETRY (close (pipe_w));
  dup2 (pipe_r, 0);
  TEMP_FAILURE_RETRY (close (pipe_r));

  dup2 (out_fd >= 0 ? out_fd : dev_null_fd, 1);
  dup2 (err_fd >= 0 ? err_fd : dev_null_fd, 2);

  if (out_fd >= 0)
    unset_cloexec_flag (1);
  if (err_fd >= 0)
    unset_cloexec_flag (2);

  if (dev_null_fd >= 0)
    TEMP_FAILURE_RETRY (close (dev_null_fd));
  if (out_fd >= 0)
    TEMP_FAILURE_RETRY (close (out_fd));
  if (err_fd >= 0)
    TEMP_FAILURE_RETRY (close (err_fd));

  if (args == NULL)
    args = tmp_args;

  if (cwd && chdir (cwd) < 0)
    _exit (EXIT_FAILURE);

  execvpe (path, args, envp);
  _exit (EXIT_FAILURE);
}

/* It changes the signals mask for the current process.  */
int
run_process_with_stdin_timeout_envp (char *path, char **args, const char *cwd, int timeout,
                                     char **envp, char *stdin, size_t stdin_len, int out_fd,
                                     int err_fd, libcrun_error_t *err)
{
  int stdin_pipe[2];
  pid_t pid;
  int ret;
  cleanup_close int pipe_r = -1;
  cleanup_close int pipe_w = -1;
  sigset_t oldmask, mask;
  int r, status;

  sigemptyset (&mask);

  ret = pipe2 (stdin_pipe, O_CLOEXEC);
  if (UNLIKELY (ret < 0))
    return crun_make_error (err, errno, "pipe");
  pipe_r = stdin_pipe[0];
  pipe_w = stdin_pipe[1];

  if (timeout > 0)
    {
      sigaddset (&mask, SIGCHLD);
      ret = sigprocmask (SIG_BLOCK, &mask, &oldmask);
      if (UNLIKELY (ret < 0))
        return crun_make_error (err, errno, "sigprocmask");
    }

  pid = fork ();
  if (UNLIKELY (pid < 0))
    {
      ret = crun_make_error (err, errno, "fork");
      goto restore_sig_mask_and_exit;
    }

  if (pid == 0)
    {
      /* run_process_child doesn't return.  */
      run_process_child (path, args, cwd, envp, pipe_r, pipe_w, out_fd, err_fd);
    }

  close_and_reset (&pipe_r);

  ret = TEMP_FAILURE_RETRY (write (pipe_w, stdin, stdin_len));
  if (UNLIKELY (ret < 0))
    {
      /* Ignore EPIPE as the container process could have already
         been terminated.  */
      if (errno != EPIPE)
        {
          ret = crun_make_error (err, errno, "write to pipe");
          goto restore_sig_mask_and_exit;
        }
    }

  close_and_reset (&pipe_w);

  if (timeout)
    {
      time_t start = time (NULL);
      time_t now;
      for (now = start; now - start < timeout; now = time (NULL))
        {
          siginfo_t info;
          int elapsed = now - start;
          struct timespec ts_timeout = { .tv_sec = timeout - elapsed, .tv_nsec = 0 };

          ret = sigtimedwait (&mask, &info, &ts_timeout);
          if (UNLIKELY (ret < 0 && errno != EAGAIN))
            {
              ret = crun_make_error (err, errno, "sigtimedwait");
              goto restore_sig_mask_and_exit;
            }

          if (info.si_signo == SIGCHLD && info.si_pid == pid)
            goto read_waitpid;

          if (ret < 0 && errno == EAGAIN)
            goto timeout;
        }
    timeout:
      kill (pid, SIGKILL);

      ret = crun_make_error (err, 0, "timeout expired for `%s`", path);
      goto restore_sig_mask_and_exit;
    }

read_waitpid:
  r = waitpid_ignore_stopped (pid, &status, 0);
  if (r < 0)
    ret = crun_make_error (err, errno, "waitpid");
  else
    ret = get_process_exit_status (status);

  /* Prevent to cleanup the pid again.  */
  pid = 0;

restore_sig_mask_and_exit:
  if (timeout > 0)
    {
      /* Cleanup the zombie process.  */
      if (pid > 0)
        {
          kill (pid, SIGKILL);
          TEMP_FAILURE_RETRY (waitpid (pid, &status, 0));
        }
      r = sigprocmask (SIG_UNBLOCK, &oldmask, NULL);
      if (UNLIKELY (r < 0 && ret >= 0))
        ret = crun_make_error (err, errno, "restoring signal mask with sigprocmask");
    }
  return ret;
}

int
mark_or_close_fds_ge_than (int n, bool close_now, libcrun_error_t *err)
{
  cleanup_close int cfd = -1;
  cleanup_dir DIR *dir = NULL;
  int ret;
  int fd;
  struct dirent *next;

  ret = syscall_close_range (n, UINT_MAX, close_now ? 0 : CLOSE_RANGE_CLOEXEC);
  if (ret == 0)
    return 0;
  if (ret < 0 && errno != EINVAL && errno != ENOSYS && errno != EPERM)
    return crun_make_error (err, errno, "close_range from `%d`", n);

  cfd = open ("/proc/self/fd", O_DIRECTORY | O_RDONLY | O_CLOEXEC);
  if (UNLIKELY (cfd < 0))
    return crun_make_error (err, errno, "open `/proc/self/fd`");

  ret = check_proc_super_magic (cfd, "/proc/self/fd", err);
  if (UNLIKELY (ret < 0))
    return ret;

  dir = fdopendir (cfd);
  if (UNLIKELY (dir == NULL))
    return crun_make_error (err, errno, "fdopendir `/proc/self/fd`");

  /* Now it is owned by dir.  */
  cfd = -1;

  fd = dirfd (dir);
  for (next = readdir (dir); next; next = readdir (dir))
    {
      int val;
      const char *name = next->d_name;
      if (name[0] == '.')
        continue;

      val = strtoll (name, NULL, 10);
      if (val < n || val == fd)
        continue;

      if (close_now)
        {
          ret = close (val);
          if (UNLIKELY (ret < 0))
            return crun_make_error (err, errno, "close fd `%d`", val);
        }
      else
        {
          ret = fcntl (val, F_SETFD, FD_CLOEXEC);
          if (UNLIKELY (ret < 0))
            return crun_make_error (err, errno, "cannot set CLOEXEC for fd `%d`", val);
        }
    }
  return 0;
}

void
get_current_timestamp (char *out, size_t len)
{
  struct timeval tv;
  struct tm now;
  char timestamp[64];

  gettimeofday (&tv, NULL);
  gmtime_r (&tv.tv_sec, &now);
  strftime (timestamp, sizeof (timestamp), "%Y-%m-%dT%H:%M:%S", &now);

  (void) snprintf (out, len, "%s.%06lldZ", timestamp, (long long int) tv.tv_usec);
  out[len - 1] = '\0';
}

int
set_blocking_fd (int fd, bool blocking, libcrun_error_t *err)
{
  int ret, flags = fcntl (fd, F_GETFL, 0);
  if (UNLIKELY (flags < 0))
    return crun_make_error (err, errno, "fcntl");

  ret = fcntl (fd, F_SETFL, blocking ? flags & ~O_NONBLOCK : flags | O_NONBLOCK);
  if (UNLIKELY (ret < 0))
    return crun_make_error (err, errno, "fcntl");
  return 0;
}

int
parse_json_file (yajl_val *out, const char *jsondata, struct parser_context *ctx arg_unused, libcrun_error_t *err)
{
  char errbuf[1024];

  *err = NULL;

  *out = yajl_tree_parse (jsondata, errbuf, sizeof (errbuf));
  if (*out == NULL)
    return crun_make_error (err, 0, "cannot parse the data: `%s`", errbuf);

  return 0;
}

#define CHECK_ACCESS_NOT_EXECUTABLE 1
#define CHECK_ACCESS_NOT_REGULAR 2

/* check that the specified path exists and it is executable.
   Return
   - 0 if the file is executable
   - CHECK_ACCESS_NOT_EXECUTABLE if it exists but it is not executable
   - CHECK_ACCESS_NOT_REGULAR if it not a regular file
   - -errno for any other generic error
 */
static int
check_access (const char *path)
{
  int ret;
  mode_t mode;

#ifdef HAVE_EACCESS
#  define ACCESS(path, mode) (eaccess (path, mode))
#else
#  define ACCESS(path, mode) (access (path, mode))
#endif

  ret = ACCESS (path, X_OK);
  if (ret < 0)
    {
      /* If the file is not executable, check if it exists.  */
      if (errno == EACCES)
        {
          int saved_errno = errno;
          ret = ACCESS (path, F_OK);
          errno = saved_errno;

          if (ret == 0)
            return CHECK_ACCESS_NOT_EXECUTABLE;
        }
      return -errno;
    }

  ret = get_file_type (&mode, false, path);
  if (UNLIKELY (ret < 0))
    return -errno;

  if (! S_ISREG (mode))
    return CHECK_ACCESS_NOT_REGULAR;

  /* It exists, is executable and is a regular file.  */
  return 0;
}

int
find_executable (char **out, const char *executable_path, const char *cwd, libcrun_error_t *err)
{
  cleanup_free char *cwd_executable_path = NULL;
  cleanup_free char *tmp = NULL;
  int last_error = -ENOENT;
  char *it, *end;
  int ret;

  *out = NULL;

  if (executable_path == NULL || executable_path[0] == '\0')
    return crun_make_error (err, ENOENT, "cannot find `` in $PATH");

  if (executable_path[0] != '/' && strchr (executable_path, '/'))
    {
      cleanup_free char *cwd_allocated = NULL;

      if (cwd == NULL)
        {
          cwd_allocated = getcwd (NULL, 0);
          if (cwd_allocated == NULL)
            OOM ();

          cwd = cwd_allocated;
        }

      /* Make sure the path starts with a '/' so it will hit the check
         for absolute paths.  */
      ret = append_paths (&cwd_executable_path, err, "/", cwd, executable_path, NULL);
      if (UNLIKELY (ret < 0))
        return ret;
      executable_path = cwd_executable_path;
    }

  /* Absolute path.  It doesn't need to lookup $PATH.  */
  if (executable_path[0] == '/')
    {
      ret = check_access (executable_path);
      if (LIKELY (ret == 0))
        {
          *out = xstrdup (executable_path);
          return 0;
        }
      last_error = ret;
      goto fail;
    }

  end = tmp = xstrdup (getenv ("PATH"));

  while ((it = strsep (&end, ":")))
    {
      cleanup_free char *path = NULL;

      if (it == end)
        it = ".";

      ret = append_paths (&path, err, it, executable_path, NULL);
      if (UNLIKELY (ret < 0))
        {
          crun_error_release (err);
          continue;
        }

      ret = check_access (path);
      if (ret == 0)
        {
          /* Change owner.  */
          *out = path;
          path = NULL;
          return 0;
        }

      if (ret == -ENOENT)
        continue;

      last_error = ret;
    }

fail:
  switch (last_error)
    {
    case CHECK_ACCESS_NOT_EXECUTABLE:
      return crun_make_error (err, EPERM, "the path `%s` exists but it is not executable", executable_path);

    case CHECK_ACCESS_NOT_REGULAR:
      return crun_make_error (err, EPERM, "the path `%s` is not a regular file", executable_path);

    default:
      errno = -last_error;
      if (errno == ENOENT)
        return crun_make_error (err, errno, "executable file `%s` not found%s", executable_path, executable_path[0] == '/' ? "" : " in $PATH");
      return crun_make_error (err, errno, "open `%s`", executable_path);
    }
}

#ifdef HAVE_FGETXATTR

static ssize_t
safe_read_xattr (char **ret, int sfd, const char *srcname, const char *name, size_t initial_size, libcrun_error_t *err)
{
  cleanup_free char *buffer = NULL;
  ssize_t current_size;
  ssize_t s;

  current_size = (ssize_t) initial_size;
  buffer = xmalloc (current_size + 1);

  while (1)
    {
      s = fgetxattr (sfd, name, buffer, current_size);
      if (UNLIKELY (s < 0))
        return crun_make_error (err, errno, "get xattr `%s` from `%s`", name, srcname);

      if (s < current_size)
        break;

      current_size *= 2;
      buffer = xrealloc (buffer, current_size + 1);
    }

  if (s <= 0)
    return s;

  buffer[s] = '\0';

  /* Change owner.  */
  *ret = buffer;
  buffer = NULL;

  return s;
}

static ssize_t
copy_xattr (int sfd, int dfd, const char *srcname, const char *destname, libcrun_error_t *err)
{
  cleanup_free char *buf = NULL;
  ssize_t xattr_len;
  char *it;

  xattr_len = flistxattr (sfd, NULL, 0);
  if (UNLIKELY (xattr_len < 0))
    {
      if (errno == ENOTSUP)
        return 0;

      return crun_make_error (err, errno, "flistxattr `%s`", srcname);
    }

  if (xattr_len == 0)
    return 0;

  buf = xmalloc (xattr_len + 1);

  xattr_len = flistxattr (sfd, buf, xattr_len + 1);
  if (UNLIKELY (xattr_len < 0))
    return crun_make_error (err, errno, "flistxattr `%s`", srcname);

  for (it = buf; it - buf < xattr_len; it += strlen (it) + 1)
    {
      cleanup_free char *v = NULL;
      ssize_t s;

      s = safe_read_xattr (&v, sfd, srcname, it, 256, err);
      if (UNLIKELY (s < 0))
        return s;

      s = fsetxattr (dfd, it, v, s, 0);
      if (UNLIKELY (s < 0))
        {
          if (errno == EINVAL || errno == EOPNOTSUPP)
            continue;

          return crun_make_error (err, errno, "fsetxattr `%s` to `%s`", it, destname);
        }
    }

  return 0;
}

#endif

static int
copy_rec_stat_file_at (int dfd, const char *path, mode_t *mode, off_t *size, dev_t *rdev, uid_t *uid, gid_t *gid)
{
  struct stat st;
  int ret;

#ifdef HAVE_STATX
  struct statx stx = {
    0,
  };

  ret = statx (dfd, path, AT_SYMLINK_NOFOLLOW | AT_STATX_DONT_SYNC,
               STATX_TYPE | STATX_MODE | STATX_SIZE | STATX_UID | STATX_GID, &stx);
  if (UNLIKELY (ret < 0))
    {
      if (errno == ENOSYS || errno == EINVAL)
        goto fallback;

      return ret;
    }

  *mode = stx.stx_mode;
  *size = stx.stx_size;
  *rdev = makedev (stx.stx_rdev_major, stx.stx_rdev_minor);
  *uid = stx.stx_uid;
  *gid = stx.stx_gid;

  return ret;

fallback:
#endif
  ret = fstatat (dfd, path, &st, AT_SYMLINK_NOFOLLOW);

  *mode = st.st_mode;
  *size = st.st_size;
  *rdev = st.st_rdev;
  *uid = st.st_uid;
  *gid = st.st_gid;

  return ret;
}

int
copy_recursive_fd_to_fd (int srcdirfd, int dfd, const char *srcname, const char *destname, libcrun_error_t *err)
{
  cleanup_close int destdirfd = dfd;
  cleanup_dir DIR *dsrcfd = NULL;
  struct dirent *de;

  dsrcfd = fdopendir (srcdirfd);
  if (UNLIKELY (dsrcfd == NULL))
    {
      TEMP_FAILURE_RETRY (close (srcdirfd));
      return crun_make_error (err, errno, "open directory `%s`", destname);
    }

  for (de = readdir (dsrcfd); de; de = readdir (dsrcfd))
    {
      cleanup_close int srcfd = -1;
      cleanup_close int destfd = -1;
      cleanup_free char *target_buf = NULL;
      int ret;
      mode_t mode;
      off_t st_size;
      dev_t rdev;
      uid_t uid;
      gid_t gid;

      if (strcmp (de->d_name, ".") == 0 || strcmp (de->d_name, "..") == 0)
        continue;

      ret = copy_rec_stat_file_at (dirfd (dsrcfd), de->d_name, &mode, &st_size, &rdev, &uid, &gid);
      if (UNLIKELY (ret < 0))
        return crun_make_error (err, errno, "stat `%s/%s`", srcname, de->d_name);

      switch (mode & S_IFMT)
        {
        case S_IFREG:
          srcfd = openat (dirfd (dsrcfd), de->d_name, O_NONBLOCK | O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
          if (UNLIKELY (srcfd < 0))
            return crun_make_error (err, errno, "open `%s/%s`", srcname, de->d_name);

          destfd = openat (destdirfd, de->d_name, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0777);
          if (UNLIKELY (destfd < 0))
            return crun_make_error (err, errno, "open `%s/%s`", destname, de->d_name);

          ret = copy_from_fd_to_fd (srcfd, destfd, 1, err);
          if (UNLIKELY (ret < 0))
            return ret;

#ifdef HAVE_FGETXATTR
          ret = (int) copy_xattr (srcfd, destfd, de->d_name, de->d_name, err);
          if (UNLIKELY (ret < 0))
            return ret;
#endif

          TEMP_FAILURE_RETRY (close (destfd));
          destfd = -1;
          break;

        case S_IFDIR:
          ret = mkdirat (destdirfd, de->d_name, mode);
          if (UNLIKELY (ret < 0))
            return crun_make_error (err, errno, "mkdir `%s/%s`", destname, de->d_name);

          srcfd = openat (dirfd (dsrcfd), de->d_name, O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
          if (UNLIKELY (srcfd < 0))
            return crun_make_error (err, errno, "open directory `%s/%s`", srcname, de->d_name);

          destfd = openat (destdirfd, de->d_name, O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
          if (UNLIKELY (destfd < 0))
            return crun_make_error (err, errno, "open directory `%s/%s`", srcname, de->d_name);

#ifdef HAVE_FGETXATTR
          ret = (int) copy_xattr (srcfd, destfd, de->d_name, de->d_name, err);
          if (UNLIKELY (ret < 0))
            return ret;
#endif

          ret = copy_recursive_fd_to_fd (srcfd, destfd, de->d_name, de->d_name, err);
          srcfd = destfd = -1;
          if (UNLIKELY (ret < 0))
            return ret;
          break;

        case S_IFLNK:
          ret = safe_readlinkat (dirfd (dsrcfd), de->d_name, &target_buf, st_size, err);
          if (UNLIKELY (ret < 0))
            return ret;

          ret = symlinkat (target_buf, destdirfd, de->d_name);
          if (UNLIKELY (ret < 0))
            return crun_make_error (err, errno, "symlinkat `%s/%s`", destname, de->d_name);
          break;

        case S_IFBLK:
        case S_IFCHR:
        case S_IFIFO:
        case S_IFSOCK:
          ret = mknodat (destdirfd, de->d_name, mode, rdev);
          if (UNLIKELY (ret < 0))
            return crun_make_error (err, errno, "mknodat `%s/%s`", destname, de->d_name);
          break;
        }

      ret = fchownat (destdirfd, de->d_name, uid, gid, AT_SYMLINK_NOFOLLOW);
      if (UNLIKELY (ret < 0))
        return crun_make_error (err, errno, "fchownat `%s/%s`", destname, de->d_name);

      /*
       * ALLPERMS is not defined by POSIX
       */
#ifndef ALLPERMS
#  define ALLPERMS (S_ISUID | S_ISGID | S_ISVTX | S_IRWXU | S_IRWXG | S_IRWXO)
#endif

      ret = fchmodat (destdirfd, de->d_name, mode & ALLPERMS, AT_SYMLINK_NOFOLLOW);
      if (UNLIKELY (ret < 0))
        {
          /* If the operation fails with ENOTSUP we are dealing with a symlink, so ignore it.  */
          if (errno == ENOTSUP)
            continue;

          return crun_make_error (err, errno, "fchmodat `%s/%s`", destname, de->d_name);
        }
    }

  return 0;
}

const char *
find_annotation (libcrun_container_t *container, const char *name)
{
  if (container->container_def->annotations == NULL)
    return NULL;

  return find_string_map_value (container->annotations, name);
}

int
safe_write (int fd, const char *fname, const void *buf, size_t count, libcrun_error_t *err)
{
  size_t written = 0;
  while (written < count)
    {
      ssize_t w = write (fd, buf + written, count - written);
      if (UNLIKELY (w < 0))
        {
          if (errno == EINTR || errno == EAGAIN)
            continue;
          return crun_make_error (err, errno, "write to `%s`", fname);
        }
      written += w;
    }
  return 0;
}

int
append_paths (char **out, libcrun_error_t *err, ...)
{
  const size_t MAX_PARTS = 32;
  const char *parts[MAX_PARTS];
  size_t sizes[MAX_PARTS];
  size_t total_len = 0;
  size_t n_parts = 0;
  size_t copied = 0;
  va_list ap;
  size_t i;

  va_start (ap, err);
  for (;;)
    {
      const char *part;
      size_t size;

      part = va_arg (ap, const char *);
      if (part == NULL)
        break;

      if (n_parts == MAX_PARTS)
        {
          va_end (ap);
          return crun_make_error (err, EINVAL, "too many paths specified");
        }

      if (n_parts == 0)
        {
          /* For the first component allow only one '/'.  */
          while (part[0] == '/' && part[1] == '/')
            part++;
        }
      else
        {
          /* And drop any initial '/' for other components.  */
          while (part[0] == '/')
            part++;
        }

      size = strlen (part);
      if (size == 0)
        continue;

      while (size > 1 && part[size - 1] == '/')
        size--;

      parts[n_parts] = part;
      sizes[n_parts] = size;

      n_parts++;
    }
  va_end (ap);

  total_len = n_parts + 1;
  for (i = 0; i < n_parts; i++)
    total_len += sizes[i];

  *out = xmalloc (total_len);

  copied = 0;
  for (i = 0; i < n_parts; i++)
    {
      bool has_trailing_slash;

      has_trailing_slash = copied > 0 && (*out)[copied - 1] == '/';
      if (i > 0 && ! has_trailing_slash)
        {
          (*out)[copied] = '/';
          copied += 1;
        }

      memcpy (*out + copied, parts[i], sizes[i]);
      copied += sizes[i];
    }
  (*out)[copied] = '\0';
  return 0;
}

#if __has_attribute(__nonstring__)
#  define __nonstring __attribute__ ((__nonstring__))
#else
#  define __nonstring
#endif

/* Adapted from mailutils 0.6.91 (distributed under LGPL 2.0+)  */
static int
b64_input (char c)
{
  const char table[64] __nonstring = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  int i;

  for (i = 0; i < 64; i++)
    {
      if (table[i] == c)
        return i;
    }
  return -1;
}

int
base64_decode (const char *iptr, size_t isize, char *optr, size_t osize, size_t *nbytes)
{
  int i = 0, tmp = 0, pad = 0;
  size_t consumed = 0;
  unsigned char data[4];

  *nbytes = 0;
  while (consumed < isize && (*nbytes) + 3 < osize)
    {
      while ((i < 4) && (consumed < isize))
        {
          tmp = b64_input (*iptr++);
          consumed++;
          if (tmp != -1)
            data[i++] = tmp;
          else if (*(iptr - 1) == '=')
            {
              data[i++] = '\0';
              pad++;
            }
        }

      /* I have an entire block of data 32 bits get the output data.  */
      if (i == 4)
        {
          *optr++ = (data[0] << 2) | ((data[1] & 0x30) >> 4);
          *optr++ = ((data[1] & 0xf) << 4) | ((data[2] & 0x3c) >> 2);
          *optr++ = ((data[2] & 0x3) << 6) | data[3];
          (*nbytes) += 3 - pad;
        }
      else
        {
          /* I did not get all the data.  */
          consumed -= i;
          return consumed;
        }
      i = 0;
    }
  return consumed;
}

char *
get_user_name (uid_t uid)
{
  struct passwd pd;
  struct passwd *temp_result_ptr;
  char pwdbuffer[200];
  if (! getpwuid_r (uid, &pd, pwdbuffer, sizeof (pwdbuffer), &temp_result_ptr))
    return xstrdup (pd.pw_name);
  return xstrdup ("");
}

int
has_suffix (const char *str, const char *suffix)
{
  if (! str || ! suffix)
    return 0;
  size_t lenstr = strlen (str);
  size_t lensuffix = strlen (suffix);
  if (lensuffix > lenstr)
    return 0;
  return memcmp (str + lenstr - lensuffix, suffix, lensuffix) == 0;
}

char *
str_join_array (int offset, size_t size, char *const array[], const char *joint)
{
  size_t jlen;
  cleanup_free size_t *lens = NULL;
  size_t i, total_size = (size - 1) * (jlen = strlen (joint)) + 1;
  char *result, *p;

  lens = xmalloc (size * sizeof (*lens));

  for (i = 0; i < size; ++i)
    {
      total_size += (lens[i] = strlen (array[i]));
    }
  p = result = xmalloc (total_size);
  for (i = offset; i < size; ++i)
    {
      memcpy (p, array[i], lens[i]);
      p += lens[i];
      if (i < size - 1)
        {
          memcpy (p, joint, jlen);
          p += jlen;
        }
    }
  *p = '\0';
  return result;
}

int
libcrun_mmap (struct libcrun_mmap_s **ret, void *addr, size_t length,
              int prot, int flags, int fd, off_t offset,
              libcrun_error_t *err)
{
  struct libcrun_mmap_s *mmap_s = NULL;

  void *mapped = mmap (addr, length, prot, flags, fd, offset);
  if (mapped == MAP_FAILED)
    return crun_make_error (err, errno, "mmap");

  mmap_s = xmalloc (sizeof (struct libcrun_mmap_s));
  mmap_s->addr = mapped;
  mmap_s->length = length;

  *ret = mmap_s;

  return 0;
}

int
libcrun_munmap (struct libcrun_mmap_s *mmap, libcrun_error_t *err)
{
  int ret;

  ret = munmap (mmap->addr, mmap->length);
  if (UNLIKELY (ret < 0))
    return crun_make_error (err, errno, "munmap");

  free (mmap);

  return 0;
}

static long
read_file_as_long_or_default (const char *path, long def_value)
{
  cleanup_free char *content = NULL;
  libcrun_error_t tmp_err = NULL;
  char *endptr;
  long val;
  int ret;

  ret = read_all_file (path, &content, NULL, &tmp_err);
  if (UNLIKELY (ret < 0))
    {
      crun_error_release (&tmp_err);
      return def_value;
    }

  errno = 0;
  val = strtol (content, &endptr, 10);
  if (UNLIKELY (errno))
    return def_value;
  if (endptr == content || (*endptr && *endptr != '\n'))
    return def_value;
  return val;
}

#define DEFAULT_OVERFLOW_ID 65534

uid_t
get_overflow_uid (void)
{
  static atomic_long cached_uid = -1;
  atomic_long uid = cached_uid;
  if (uid == -1)
    {
      uid = read_file_as_long_or_default ("/proc/sys/kernel/overflowuid", DEFAULT_OVERFLOW_ID);
      cached_uid = uid;
    }
  return uid;
}

gid_t
get_overflow_gid (void)
{
  static atomic_long cached_gid = -1;
  atomic_long gid = cached_gid;
  if (gid == -1)
    {
      gid = read_file_as_long_or_default ("/proc/sys/kernel/overflowgid", DEFAULT_OVERFLOW_ID);
      cached_gid = gid;
    }
  return gid;
}

void
consume_trailing_slashes (char *path)
{
  if (! path || path[0] == '\0')
    return;

  char *last = path + strlen (path);

  while (last > path && *(last - 1) == '/')
    last--;

  *last = '\0';
}

char **
read_dir_entries (const char *path, libcrun_error_t *err)
{
  cleanup_dir DIR *dir = NULL;
  size_t n_entries = 0;
  size_t entries_size = 16;
  char **entries = NULL;
  struct dirent *de;

  dir = opendir (path);
  if (UNLIKELY (dir == NULL))
    {
      crun_make_error (err, errno, "opendir `%s`", path);
      return NULL;
    }

  entries = xmalloc (entries_size * sizeof (char *));
  while ((de = readdir (dir)))
    {
      if (strcmp (de->d_name, ".") == 0 || strcmp (de->d_name, "..") == 0)
        continue;
      if (n_entries == entries_size)
        {
          entries_size *= 2;
          entries = xrealloc (entries, entries_size * sizeof (char *));
        }
      entries[n_entries++] = xstrdup (de->d_name);
    }
  entries = xrealloc (entries, (n_entries + 1) * sizeof (char *));
  entries[n_entries] = NULL;

  return entries;
}

int
cpuset_string_to_bitmask (const char *str, char **out, size_t *out_size, libcrun_error_t *err)
{
  cleanup_free char *mask = NULL;
  size_t mask_size = 0;
  const char *p = str;
  char *endptr;

  while (*p)
    {
      long long start_range, end_range;

      if (*p < '0' || *p > '9')
        goto invalid_input;

      start_range = strtoll (p, &endptr, 10);
      if (start_range < 0)
        goto invalid_input;

      p = endptr;

      if (*p != '-')
        end_range = start_range;
      else
        {
          p++;

          if (*p < '0' || *p > '9')
            goto invalid_input;

          end_range = strtoll (p, &endptr, 10);

          if (end_range < start_range)
            goto invalid_input;

          p = endptr;
        }

      /* Just set some limit.  */
      if (end_range > (1 << 20))
        goto invalid_input;

      if (end_range >= (long long) (mask_size * CHAR_BIT))
        {
          size_t new_mask_size = (end_range / CHAR_BIT) + 1;
          mask = xrealloc (mask, new_mask_size);
          memset (mask + mask_size, 0, new_mask_size - mask_size);
          mask_size = new_mask_size;
        }

      for (long long i = start_range; i <= end_range; i++)
        mask[i / CHAR_BIT] |= (1 << (i % CHAR_BIT));

      if (*p == ',')
        p++;
      else if (*p)
        goto invalid_input;
    }

  *out = mask;
  mask = NULL;
  *out_size = mask_size;

  return 0;

invalid_input:
  return crun_make_error (err, 0, "cannot parse input `%s`", str);
}

struct channel_fd_pair
{
  struct ring_buffer *rb;

  int in_fd;
  int out_fd;

  int infd_epoll_events;
  int outfd_epoll_events;
};

struct channel_fd_pair *
channel_fd_pair_new (int in_fd, int out_fd, size_t size)
{
  struct channel_fd_pair *channel = xmalloc (sizeof (struct channel_fd_pair));
  channel->in_fd = in_fd;
  channel->out_fd = out_fd;
  channel->infd_epoll_events = -1;
  channel->outfd_epoll_events = -1;
  channel->rb = ring_buffer_make (size);
  return channel;
}

void
channel_fd_pair_free (struct channel_fd_pair *channel)
{
  if (channel == NULL)
    return;

  ring_buffer_free (channel->rb);
  free (channel);
}

int
channel_fd_pair_process (struct channel_fd_pair *channel, int epollfd, libcrun_error_t *err)
{
  bool is_input_eagain = false, is_output_eagain = false, repeat;
  int ret, i;

  /* This function is called from an epoll loop.  Use a hard limit to avoid infinite loops
     and prevent other events from being processed.  */
  for (i = 0, repeat = true; i < 1000 && repeat; i++)
    {
      repeat = false;
      if (ring_buffer_get_space_available (channel->rb) > 0)
        {
          ret = ring_buffer_read (channel->rb, channel->in_fd, &is_input_eagain, err);
          if (UNLIKELY (ret < 0))
            return ret;
          if (ret > 0)
            repeat = true;
        }
      if (ring_buffer_get_data_available (channel->rb) > 0)
        {
          ret = ring_buffer_write (channel->rb, channel->out_fd, &is_output_eagain, err);
          if (UNLIKELY (ret < 0))
            return ret;
          if (ret > 0)
            repeat = true;
        }
    }

  if (epollfd >= 0)
    {
      size_t available = ring_buffer_get_space_available (channel->rb);
      size_t used = ring_buffer_get_data_available (channel->rb);
      int events;

      /* If there is space available in the buffer, we want to read more.  */
      events = (available > 0) ? (EPOLLIN | (is_input_eagain ? EPOLLET : 0)) : 0;
      if (events != channel->infd_epoll_events)
        {
          ret = epoll_helper_toggle (epollfd, channel->in_fd, events, err);
          if (UNLIKELY (ret < 0))
            return ret;
          channel->infd_epoll_events = events;
        }

      /* If there is data available in the buffer, we want to write as soon as
         it is possible.  */
      events = (used > 0) ? (EPOLLOUT | (is_output_eagain ? EPOLLET : 0)) : 0;
      if (events != channel->outfd_epoll_events)
        {
          ret = epoll_helper_toggle (epollfd, channel->out_fd, events, err);
          if (UNLIKELY (ret < 0))
            return ret;
          channel->outfd_epoll_events = events;
        }
    }
  return 0;
}
