/* remote filesystem based on netfs

   Copyright (C) 2020. Junling Ma <junlingm@gmail.com>

   This file is part of the GNU Hurd.

   The GNU Hurd is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License as
   published by the Free Software Foundation; either version 2, or (at
   your option) any later version.

   The GNU Hurd is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111, USA. */

#include "libvfs/vfs.h"
#include <hurd/pager.h>
#include <sys/mman.h>

char *netfs_server_name;
char *netfs_server_version;

/* return the local user that started the translator */
static error_t get_local_user(struct iouser **user)
{
  uid_t euidbuf[10], auidbuf[10], *euids = euidbuf, *auids = auidbuf;
  gid_t egidbuf[10], agidbuf[10], *egids = egidbuf, *agids = agidbuf;
  mach_msg_type_number_t euidsCnt = 10, auidsCnt = 10, egidsCnt = 10, agidsCnt = 10;
  int r = auth_getids(netfs_auth_server_port, &euids, &euidsCnt, &auids, &auidsCnt,
    &egids, &egidsCnt, &agids, &agidsCnt);
  if (r)
    return r;
  r = iohelp_create_complex_iouser(user, euids, euidsCnt, egids, egidsCnt);
  if (!r)
    {
      if (euids != euidbuf)
        munmap (euids, euidsCnt * sizeof (uid_t));
      if (egids != egidbuf)
        munmap (egids, egidsCnt * sizeof (uid_t));
      if (auids != auidbuf)
        munmap (auids, auidsCnt * sizeof (uid_t));
      if (agids != agidbuf)
        munmap (agids, agidsCnt * sizeof (uid_t));
    }
  return r;
}

/* create a vfs and return in FS. The SERVER_NAME and SERVER_VERSION will be used to fill 
 * netfs_server_name and netfs_version. 
 */
error_t vfs_create(
  const char *server_name, 
  const char *server_version,
  struct vfs_hooks *hooks,
  struct vfs **fs)
{
  /* check for required hooks */
  if (hooks->lstat == NULL || hooks->statfs == NULL ||
    hooks->open == NULL || hooks->close == NULL || hooks->read == NULL || hooks->readlink == NULL)
    return EINVAL;

  netfs_init ();
  netfs_server_name = strdup(server_name);
  netfs_server_version = strdup(server_version);

  struct iouser *local_user;
  error_t err = get_local_user(&local_user);

  if (err)
    return err;

  *fs = malloc (sizeof(**fs));
  if (*fs == NULL)
    return ENOMEM;

  hurd_ihash_init (&(*fs)->nodes, offsetof (struct netnode, node_locp));
  pthread_spin_init (&(*fs)->nodes_lock, PTHREAD_PROCESS_PRIVATE);

  (*fs)->hooks = hooks;
  (*fs)->local_user = local_user;
  pthread_spin_init(&(*fs)->pager_lock, 0);
  (*fs)->pager_port_bucket = ports_create_bucket();
  (*fs)->pager_requests = NULL;
  err = pager_start_workers ((*fs)->pager_port_bucket, &(*fs)->pager_requests);
  if (!err)
    err = vfs_create_node (*fs, NULL, 0, &(*fs)->root);
  
  if (!err)
    netfs_validate_stat((*fs)->root, (*fs)->local_user);
  
  /* if the root is a dir then dir hooks must be implemented */
  if (!err && ((*fs)->root->nn_stat.st_mode & S_IFMT) == S_IFDIR &&
    (hooks->lookup == NULL || hooks->opendir == NULL || hooks->readdir == NULL 
    || hooks->closedir == NULL))
    err = EINVAL;
  
  if (err)
    {
      iohelp_free_iouser(local_user);
      free (*fs);
      *fs = NULL;
    }
  return err;
}

/* start the netfs server loop. FLAGS is the flags for opening the undelrying node */
error_t vfs_start(struct vfs *fs, int flags)
{
  mach_port_t bootstrap, underlying_node;
  task_get_bootstrap_port (mach_task_self (), &bootstrap);

  netfs_root_node = fs->root;

  underlying_node = netfs_startup (bootstrap, flags);
  if (fs->hooks->set_underlying_node)
    {
      error_t err = fs->hooks->set_underlying_node(fs->hooks, underlying_node);
      if (err)
        return err;
    }

  for (;;)
    netfs_server_loop ();
}

/* Return a new node in NODE in vfs FS, with the parent DIR, the PATH relative to the root 
 * of the vfs, and a single reference. If DIR is not NULL, it must be locked.
 */
error_t vfs_create_node (struct vfs *fs, struct node *dir, ino_t ino, struct node **node)
{
  struct netnode *nn = malloc (sizeof (struct netnode));
  error_t err = ESUCCESS;

  if (! nn)
    return ENOMEM;

  nn->fs = fs;
  nn->file = NULL;
  nn->pager = NULL;

  *node = netfs_make_node (nn);
  if (*node == NULL)
    {
      free (nn);
      return ENOMEM;
    }

  (*node)->nn_stat.st_ino = ino;
  pthread_spin_lock (&fs->nodes_lock);
  err = hurd_ihash_add (&fs->nodes, (hurd_ihash_key_t) ino, nn);
  pthread_spin_unlock (&fs->nodes_lock);

  nn->node = *node;
  nn->dir = dir;
  if (dir) 
    netfs_nref(dir);
  return err;
}
