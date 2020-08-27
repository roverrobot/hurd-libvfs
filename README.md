# hurd-libvfs
A path based virtual file system for GNU Hurd

This library implements the libnetfs interface, based on a virtual filesystem interface defined below:

```C
struct vfs_hooks
{
  /* the required file system hookss needed to implement a readonly file system */

  /* required fsys hook */
  error_t (*lstat)(struct vfs_hooks *hooks, const char *path, struct stat *statbuf);
  error_t (*statfs)(struct vfs_hooks *hooks, const char *path, struct statfs *statbuf);

  /* required dir hooks, needed for name lookups */
  error_t (*opendir)(struct vfs_hooks *hooks, const char *path, vfs_dir_t *dir);
  /* read an DIR entry into DIRENT, which has a maximum size DIRENT_SIZE. If the maximum
   * size is not large enough to hold the entry, return EKERN_NO_SPACE. DIRENT may be 
   * NULL, in which case the entry will be skipped. ENOENT will be returned if no further 
   * entries exist */ 
  error_t (*readdir)(vfs_dir_t dir, struct dirent *dirent, size_t dirent_size);
  error_t (*closedir)(vfs_dir_t dir);

  /* required hook for reading symlinks, store the target in CONTENT, which is malloced 
   * and needs to be freed. */
  error_t (*readlink)(struct vfs_hooks *remote, const char *path, char **content);

  /* required file hooks */
  error_t (*open)(struct vfs_hooks *remote, const char *path, int flags, mode_t mode, vfs_file_t *file);
  error_t (*close)(vfs_file_t file);
  error_t (*read)(vfs_file_t file, off_t offset, void *buffer, size_t *size);

  /* optional hooks. may be NULL */

  /* optional hook to notify the vfs backend about the underlying node. If defined,
   * This is called after netfs_startup is called, but before netfs_server_loop is called.
   */
  error_t (*set_underlying_node)(struct vfs_hooks *hooks, mach_port_t underlying_node);
  /* optional hook to replace the UID and GID on a remote host by those of LOCALUSER.
   * For example, a server may ssh or ftp to a remote host with a user name and group id 
   * that differs from the local user that starts the server */
  error_t (*getuser)(struct vfs_hooks *remote, struct iouser *localuser, uid_t *uid, gid_t *gid);

  /* optional hooks, NULL if the operation is not supported */

  /* optional hook to replace the UID and GID on a remote host by those of LOCALUSER.
   * For example, a server may ssh or ftp to a remote host with a user name and group id 
   * that differs from the local user that starts the server */
  error_t (*getuser)(struct vfs_hooks *remote, struct iouser *localuser, uid_t *uid, gid_t *gid);
  
  /* optional hooks needed to change the content of a file */
  error_t (*write)(vfs_file_t file, off_t offset, const void *buffer, size_t *size);
  error_t (*utimes)(struct vfs_hooks *remote, const char *path, const struct timeval *times);

  /* optional hooks for writable files */
  error_t (*fsync)(vfs_file_t file);
  error_t (*truncate)(struct vfs_hooks *remote, const char *path, off_t offset);

  /* optional hook to sync changes of the file system */
  error_t (*syncfs)(struct vfs_hooks *remote, const char *path, struct statfs *statbuf);

  /* optional file system hooks */
  error_t (*mkdir)(struct vfs_hooks *remote, const char *path, mode_t mode);
  error_t (*rmdir)(struct vfs_hooks *remote, const char *path);
  error_t (*unlink)(struct vfs_hooks *remote, const char *path);
  error_t (*rename)(struct vfs_hooks *remote, const char *original_name, const char *new_name);
  error_t (*link)(struct vfs_hooks *remote, const char *target, const char *dest);
  error_t (*symlink)(struct vfs_hooks *remote, const char *target, const char *dest);
  error_t (*mkdev)(struct vfs_hooks *remote, const char *path, mode_t type, dev_t indexes);
  error_t (*chown) (struct vfs_hooks *remote, const char *path, uid_t uid, uid_t gid);
  error_t (*chmod)(struct vfs_hooks *remote, const char *path, mode_t mode);
  error_t (*chflags)(struct vfs_hooks *remote, const char *path, int flags);
  error_t (*chauthor)(struct vfs_hooks *remote, const char *path, uid_t author);
  error_t (*gettranslator)(struct vfs_hooks *remote, const char *path, char **argz, size_t *argzlen);
  error_t (*settranslator)(struct vfs_hooks *remote, const char *path, const char *argz, size_t argzlen);
};
```

# Usage

## Create a VFS
Use the following function to create a VFS object, which will then be passed to the ```vfs_start``` function below.

Parameters:
* server_name: the name of the server, will be the value of netfs_server_name
* server_version: the version of the server, will be the value of netfs_server_version
* hooks: the vfs hooks as defined above.
* fs: contains the newly create vfs, or NULL on failure

Return value: ESUCCESS on success, or an error code specifying the reason for the failure.
```C
error_t vfs_create(
  const char *server_name, 
  const char *server_version,
  struct vfs_hooks *hooks,
  struct vfs **fs);
```

## Start the VFS server loop

The following function starts the server loop, where ```fs``` is the vfs object created by ```vfs_create```, and flags is the flags used to open the underlying node (and is passed to netfs_start). On success, it returns ESUCCESS. On failure, if returns the error code for the reason of the failure.

```C
error_t vfs_start(struct vfs *fs, int flags);
```
