# hurd-libvfs
A virtual file system for GNU Hurd

This library implements the libnetfs interface, based on a virtual filesystem interface 
defined below: (at this state, it only contains the needed hooks for ls). The main goal 
of this library is to allow vfs writers to concentrate on providing contents, while the
library takes care of locking. In addition, the vfs interface is more akin to the glibc
syscalls.

```C
struct vfs_hooks
{
  /* the required file system hooks needed to implement a readonly file system */

  /* required fsys hook */
  error_t (*statfs)(struct vfs_hooks *hooks, struct statfs *statbuf);
  /* an inode is not used by libvfs any more. It should be dropped */
  void (*drop)(struct vfs_hooks *hooks, ino64_t ino);

  /* required file hooks */
  /* stat the inode INO and return in STATBUF, do not follow the symlink if INO is one */
  error_t (*lstat)(struct vfs_hooks *hooks, ino64_t ino, struct stat64 *statbuf);
  /* required hook for reading symlinks, store the target in CONTENT, which is malloced 
   * and needs to be freed. */
  error_t (*readlink)(struct vfs_hooks *hooks, ino64_t ino, char **content);

  /* required dir hooks if the remote path of the root is a dir, otherwise optional.
   * needed for name lookups 
   */

  /* look up a NAME in a DIR, and return the inode in INO */
  error_t (*lookup)(struct vfs_hooks *hooks, ino64_t dir, const char *name, ino64_t *ino);
  error_t (*opendir)(struct vfs_hooks *hooks, ino64_t ino, vfs_dir_t *dir);
  /* read an DIR entry into DIRENT, which has a maximum size DIRENT_SIZE. If the maximum
   * size is not large enough to hold the entry, return EKERN_NO_SPACE. DIRENT may be 
   * NULL, in which case the entry will be skipped. ENOENT will be returned if no further 
   * entries exist */ 
  error_t (*readdir)(vfs_dir_t dir, struct dirent64 *dirent, size_t dirent_size);
  error_t (*closedir)(vfs_dir_t dir);

  /* optional hooks. may be NULL */

  /* optional hook to notify the vfs backend about the underlying node. If defined,
   * This is called after netfs_startup is called, but before netfs_server_loop is called.
   */
  error_t (*set_underlying_node)(struct vfs_hooks *hooks, mach_port_t underlying_node);
  /* optional hook to replace the UID and GID on a remote host by those of LOCALUSER.
   * For example, a server may ssh or ftp to a remote host with a user name and group id 
   * that differs from the local user that starts the server */
  error_t (*getuser)(struct vfs_hooks *remote, struct iouser *localuser, uid_t *uid, gid_t *gid);
};
```

# Usage

## Installation
This library must be installed to /usr, so that passive translators can find it. That is,
```
autoreconf --install
configure --prefix=/usr
make && sudo make install
```

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
