/* Manage a virtual filesystem

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

#ifndef __LIBVFS_VFS_H__
#define __LIBVFS_VFS_H__

#include <libvfs/vfs_hooks.h>
#include <hurd/netfs.h>
#include <pthread.h>

/* libnetfs node structure. */
struct netnode
{
  struct vfs *fs;
  /* link back to the netfs node */
  struct node *node;
  /* the dir node that contains this node */
  struct node *dir;
  /* Used for removing this entry from vfs NODES hash table */
  hurd_ihash_locp_t node_locp;
  /* the remote file handle, NULL if not opened or is a dir */
  struct vfs_file *file;
};

/* a particular virtual file system */
struct vfs {
  /* the local user that started vfs */
  struct iouser *local_user;
  /* the remote fs hooks */
  struct vfs_hooks *hooks;
  /* the root node */
  struct node *root;
  /* A hash table that maps paths to opened noded.  */
  struct hurd_ihash nodes;
  /* a lock that protects the NODES hash table */
  pthread_spinlock_t nodes_lock;
};

/* Return a new node in NODE in vfs FS, with the parent DIR, the PATH relative to the root 
 * of the vfs, and a single reference. If DIR is not NULL, it must be locked.
 */
error_t vfs_create_node (
  struct vfs *fs, 
  struct node *dir, 
  ino_t ino, 
  struct node **node);

/* create a vfs and return in FS. The SERVER_NAME and SERVER_VERSION will be used to fill 
 * netfs_server_name and netfs_version. 
 */
error_t vfs_create(
  const char *server_name, 
  const char *server_version,
  struct vfs_hooks *hooks,
  struct vfs **fs);

/* setup the translator and run the server loop */
error_t vfs_start(struct vfs *fs, int flags);

#endif /* __LIBVFS_VFS_H__ */
