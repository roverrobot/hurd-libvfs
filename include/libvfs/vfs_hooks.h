/* the hooks to implement a virtual filesystem

   Copyright (C) 2020. Junling Ma <junlingm@gmail.com>

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License as
   published by the Free Software Foundation; either version 2, or (at
   your option) any later version.

   This program is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA. */

#ifndef __VFS_HOOKS_H__
#define __VFS_HOOKS_H__

#define _LARGEFILE64_SOURCE
#define _FILE_OFFSET_BITS 64
#include <dirent.h>
#define __USE_GNU
#include <errno.h>
#include <mach.h>
#include <stddef.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/vfs.h>

struct iouser;

/* Returned directory entries are aligned to blocks this many bytes long.
   Must be a power of two.  */
#define DIRENT_ALIGN 4
#define ALIGNED_LEN(len, align) (((len) + ((align)- 1)) & ~((align) - 1))

#define DIRENT_NAME_OFFS offsetof (struct dirent64, d_name)
/* Length is structure before the name + the name + '\0', all
   padded to a four-byte alignment.  */
#define DIRENT_LEN(name_len) ALIGNED_LEN(DIRENT_NAME_OFFS + (name_len) + 1, DIRENT_ALIGN)
static inline size_t dirent_len(struct dirent64 *dir)
{
  return dir ? DIRENT_LEN(dir->d_namlen) : 0;
}

/* a remote file handle. must be implemented by a specific vfs implementation. */
struct vfs_file;
typedef struct vfs_file * vfs_file_t;

/* a remote dir handle. must be implemented by a specific vfs implementation. */
struct vfs_dir;
typedef struct vfs_dir * vfs_dir_t;

/* an abstraction of a remote file system. The hooks are mostly based
 * on glibc file and dir syscall interfaces, except for read and write, which is based on
 * the gnumach device interface.
 */
struct vfs_hooks
{
  /* the required file system hooks needed to implement a readonly file system */

  /* required fsys hook */
  error_t (*statfs)(struct vfs_hooks *hooks, struct statfs *statbuf);

  /* required file hooks */
  /* stat the inode INO and return in STATBUF, do not follow the symlink if INO is one */
  error_t (*lstat)(struct vfs_hooks *hooks, ino64_t ino, struct stat64 *statbuf);
  /* required hook for reading symlinks, store the target in CONTENT, which is malloced 
   * and needs to be freed. */
  error_t (*readlink)(struct vfs_hooks *hooks, ino64_t ino, char **content);
  /* open the file with in INO with FLAGS, and return it in FILE. If the file will be 
   * created, create it with the MODE
   */
  error_t (*open)(struct vfs_hooks *remote, ino64_t ino, int flags, mode_t mode, vfs_file_t *file);
  error_t (*close)(vfs_file_t file);
  /* read the FILE from the OFFSET into BUFFER, which capacity is specified in *SIZE. THe
   * number of bytes successfully read is returned in *SIZE */ 
  error_t (*read)(vfs_file_t file, off_t offset, void *buffer, size_t *size);

  /* required dir hooks if the remote path of the root is a dir, otherwise optional.
   * needed for name lookups 
   */

  /* look up a NAME in a DIR, and return the inode in INO. returns ENOENT if name is not found */
  error_t (*lookup)(struct vfs_hooks *hooks, ino64_t dir, const char *name, ino64_t *ino);
  error_t (*opendir)(struct vfs_hooks *hooks, ino64_t ino, vfs_dir_t *dir);
  /* read an DIR entry into DIRENT, which has a maximum size DIRENT_SIZE. If the maximum
   * size is not large enough to hold the entry, return EKERN_NO_SPACE. DIRENT may be 
   * NULL, in which case the entry will be skipped. ENOENT will be returned if no further 
   * entries exist */ 
  error_t (*readdir)(vfs_dir_t dir, struct dirent64 *dirent, size_t dirent_size);
  error_t (*closedir)(vfs_dir_t dir);

  /* optional hooks. may be NULL */

  /* write BUFFER to the FILE starting at the OFFSET for a length of *SIZE bytes. The
   * number of bytes successfully written is returned in *SIZE */ 
  error_t (*write)(vfs_file_t file, off_t offset, const void *buffer, size_t *size);
  /* write buffered data of FILE to physical media */
  error_t (*fsync)(vfs_file_t file);
  /* change the size of file INO to OFFSET, truncate if shortened, and fills with 0 if enlarged */
  error_t (*truncate)(struct vfs_hooks *hooks, ino64_t ino, off_t offset);
  /* make an inode in DIR with a name NAME, a creation mode MODE, the ownership UID and GID. 
   * Note that the type of the inode is also specified by MODE. For a regular file, DATA
   * and LEN holds the content of the file. If DATA is NULL but LEN is nonzero, fill the file
   * with LEN bytes of 0. For an empty file, either FILE is NULL, or LEN is 0. For a symlink, 
   * DATA is a (const char*) holding the target, and LEN is the string length. For any other
   * file types, DATA and LEN will be ignored. */
  error_t (*mkinode)(struct vfs_hooks *hooks, ino64_t dir, const char *name, mode_t mode, 
    uid_t uid, gid_t gid, void *data, size_t len);

  /* remove the DIR */
  error_t (*rmdir)(struct vfs_hooks *hooks, ino64_t dir);
  /* unlink the file INO */
  error_t (*unlink)(struct vfs_hooks *hooks, ino64_t ino);

  /* an inode is not used by libvfs any more. It should be dropped */
  void (*drop)(struct vfs_hooks *hooks, ino64_t ino);

  /* get the passive translator for the inode INO into ARGZ, and its size in ARGZLEN */
  error_t (*gettranslator)(struct vfs_hooks *remote, ino64_t ino, char **argz, size_t *argzlen);
  /* set the passive translator for the inode INO as ARGZ, with its length in ARGZLEN */
  error_t (*settranslator)(struct vfs_hooks *remote, ino64_t ino, const char *argz, size_t argzlen);

  /* optional hook to notify the vfs backend about the underlying node. If defined,
   * This is called after netfs_startup is called, but before netfs_server_loop is called.
   */
  error_t (*set_underlying_node)(struct vfs_hooks *hooks, mach_port_t underlying_node);
};

#endif