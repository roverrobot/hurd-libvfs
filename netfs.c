/* Implementation of the netfs interface

   Copyright (C) 2020, Junling Ma <junlingm@gmail.com>

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

#include <libvfs/vfs.h>

#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>

/* helper functions */

/* Node NP is all done; free all its associated storage. */
void
netfs_node_norefs (struct node *node)
{
  struct netnode *nn = node->nn;
  /* Remove this entry from the set of known inodes.  */
  pthread_spin_lock (&nn->fs->nodes_lock);
  hurd_ihash_locp_remove (&nn->fs->nodes, nn->node_locp);
  pthread_spin_unlock (&nn->fs->nodes_lock);

  if (nn->dir)
    netfs_nrele(nn->dir);

  nn->fs->hooks->drop(nn->fs->hooks, node->nn_stat.st_ino);
  free (nn);
  free (node);
}

/* node management */

/* Make sure that NP->nn_stat is filled with current information.  CRED
   identifies the user responsible for the operation.  */
error_t
netfs_validate_stat (struct node *node, struct iouser *cred)
{
  struct vfs *fs = node->nn->fs;
  struct vfs_hooks *hooks = fs->hooks;
  struct stat64 statbuf;
  error_t err = hooks->lstat(hooks, node->nn_stat.st_ino, &statbuf);
  if (!err)
    {
      memcpy(&node->nn_stat, &statbuf, sizeof(statbuf));
      node->nn_translated = node->nn_stat.st_mode;
      /*map remote user to local user, otherwise the uid and gid is -1 */
      if (hooks->getuser)
        err = hooks->getuser(hooks, fs->local_user, &node->nn_stat.st_uid, &node->nn_stat.st_gid);
    }
  return err;
}

/* name lookup */

/* Lookup NAME in DIR for USER; set *NODE to the found name upon return.  If
   the name was not found, then return ENOENT.  On any error, clear *NODE.
   (*NODE, if found, should be locked, this call should unlock DIR no matter
   what.) */
error_t netfs_attempt_lookup (struct iouser *user, struct node *dir,
			      char *name, struct node **node)
{
  struct vfs *fs = dir->nn->fs;
  struct vfs_hooks *hooks = fs->hooks;
  *node = NULL;
  if ((dir->nn_stat.st_mode & S_IFMT) != S_IFDIR)
    {
      pthread_mutex_unlock (&dir->lock);
      return ENOTDIR;
    }
    
  if (*name == '\0' || strcmp (name, ".") == 0)
    /* Current directory -- just add an additional reference to DIR's node
       and return it.  */
    {
      netfs_nref (dir);
      *node = dir;
      return 0;
    }

  error_t err = ESUCCESS;
  if (strcmp (name, "..") == 0)
    /* Parent directory.  */
    {
      if (dir->nn->dir)
        {
          *node = dir->nn->dir;
          pthread_mutex_lock (&(*node)->lock);
          netfs_nref (*node);
        }
      else
        {
          *node = 0;
          err = ENOENT;
      	}
      pthread_mutex_unlock (&dir->lock);
      return err;
    }

  ino_t ino;
  err = hooks->lookup(hooks, dir->nn_stat.st_ino, name, &ino);
  if (err)
    return err;

  /* check if the node is in the cache */
  pthread_spin_lock (&fs->nodes_lock);
  struct netnode *nn = hurd_ihash_find (&fs->nodes, ino);
  pthread_spin_unlock (&fs->nodes_lock);

  /* check if the file exists */
  struct stat64 statbuf;
  err = hooks->lstat(hooks, ino, &statbuf);
  if (err)
    {
      pthread_mutex_unlock (&dir->lock);
      return err;
    }

  if (nn != NULL)
    *node = nn->node;
  else
    err = vfs_create_node(fs, dir, ino, node);

  pthread_mutex_lock (&(*node)->lock);
  netfs_nref (*node);
  memcpy(&(*node)->nn_stat, &statbuf, sizeof(statbuf));
  (*node)->nn_translated = statbuf.st_mode;

  /* check for passive translator */
  if (!err)
    {
      char *argz;
      size_t argz_len;
      err = netfs_get_translator(*node, &argz, &argz_len);
      if (err == ESUCCESS && argz_len > 0)
        {
          (*node)->nn_stat.st_mode |= S_IPTRANS;
          (*node)->nn_translated = (*node)->nn_stat.st_mode & (S_IPTRANS | S_IFMT);
        }
      else if (err == ENOENT || err == ENOTSUP || argz_len == 0)
        {
          (*node)->nn_stat.st_mode &= ~S_IPTRANS;
          (*node)->nn_translated = (*node)->nn_stat.st_mode & (S_IPTRANS | S_IFMT);
          err = ESUCCESS;
        }
        err = ESUCCESS;
    }

  pthread_mutex_unlock (&dir->lock);
  return err;
}

/* This should attempt to fetch filesystem status information for the remote
   filesystem, for the user CRED. */
error_t
netfs_attempt_statfs (struct iouser *cred, struct node *node,
		      struct statfs *st)
{
  struct vfs_hooks *hooks = node->nn->fs->hooks;
  return hooks->statfs(hooks, st);
}

/* This should sync the entire remote filesystem.  If WAIT is set, return
   only after sync is completely finished.  */
error_t netfs_attempt_syncfs (struct iouser *cred, int wait)
{
  return 0;
}

/* The granularity with which we allocate space to return our result.  */
#define DIRENTS_CHUNK_SIZE	(8*1024)

/* The user must define this function.  Fill the array *DATA of size
   BUFSIZE with up to NENTRIES dirents from DIR (which is locked)
   starting with entry ENTRY for user CRED.  The number of entries in
   the array is stored in *AMT and the number of bytes in *DATACNT.
   If the supplied buffer is not large enough to hold the data, it
   should be grown.  */
error_t
netfs_get_dirents (struct iouser *cred, struct node *dir,
		   int first_entry, int max_entries, char **data,
		   mach_msg_type_number_t *data_len,
		   vm_size_t max_data_len, int *data_entries)
{
  if ((dir->nn_stat.st_mode & S_IFMT) != S_IFDIR)
    return ENOTDIR;

  struct vfs *fs = dir->nn->fs;
  struct vfs_hooks *hooks = fs->hooks;
  *data = NULL;
  *data_len = 0;
  *data_entries = 0;
  
  struct vfs_dir *rdir;
  error_t err = hooks->opendir(hooks, dir->nn_stat.st_ino, &rdir);
  if (err)
    return err;
  if (rdir == NULL)
    return EIO;

  /* skip the unwanted entries */
  while (first_entry-- > 0)
    {
      err = hooks->readdir(rdir, NULL, 0);
      if (err)
        {
          if (err == ENOENT)
            err = ESUCCESS;
          hooks->closedir(rdir); 
          return err;
        }
    }

  size_t size = DIRENTS_CHUNK_SIZE;
  /* the initial buffer, which may grow as more entries are read */
  char *p = *data = mmap(0, size, PROT_READ|PROT_WRITE, MAP_ANON, 0, 0);
  size_t ent_len;
  while (max_entries == -1 || max_entries-- > 0)
    {
      /* expand the data buffer until an entry is read in */
      for(;;)
        {
          err = hooks->readdir(rdir, (struct dirent64*)p, size - *data_len);
          if (err != EKERN_NO_SPACE)
            break;

          vm_address_t extension = (vm_address_t)(*data + size);
          err = vm_allocate (mach_task_self (), &extension, DIRENTS_CHUNK_SIZE, 0);
          if (err)
            break;
          size += DIRENTS_CHUNK_SIZE;
        }
      if (err)
        break;

      ent_len = dirent_len((struct dirent64*)p);
      p += ent_len;
      *data_len += ent_len;
      ++*data_entries;
    }

  /* if no further entries exist, hooks->readdir returns ENOENT */
  if (err == ENOENT)
    err = ESUCCESS;
  hooks->closedir(rdir);
  return err;
}

/* Node NODE is being opened by USER, with FLAGS.  NEWNODE is nonzero if we
   just created this node.  Return an error if we should not permit the open
   to complete because of a permission restriction. */
error_t
netfs_check_open_permissions (struct iouser *user, struct node *node,
			      int flags, int newnode)
{
  error_t err = ESUCCESS;
  if (!err && (flags & O_READ))
    err = fshelp_access (&node->nn_stat, S_IREAD, user);
  if (!err && (flags & O_WRITE))
    err = fshelp_access (&node->nn_stat, S_IWRITE, user);
  if (!err && (flags & O_EXEC))
    err = fshelp_access (&node->nn_stat, S_IEXEC, user);
  return err;
}

/* Attempt to create a file named NAME in DIR for USER with MODE.  Set *NODE
   to the new node upon return.  On any error, clear *NODE.  *NODE should be
   locked on success; no matter what, unlock DIR before returning.  */
error_t
netfs_attempt_create_file (struct iouser *user, struct node *dir,
			   char *name, mode_t mode, struct node **node)
{
  return ENOTSUP;
}

/* This should attempt a utimes call for the user specified by CRED on node
   NODE, to change the atime to ATIME and the mtime to MTIME. */
error_t
netfs_attempt_utimes (struct iouser *cred, struct node *node,
		      struct timespec *atime, struct timespec *mtime)
{
  return ENOTSUP;
}

/* Return the valid access types (bitwise OR of O_READ, O_WRITE, and O_EXEC)
   in *TYPES for file NODE and user CRED.  */
error_t
netfs_report_access (struct iouser *cred, struct node *node, int *types)
{
  return ENOTSUP;
}

/* Trivial definitions.  */


/* This should sync the file NODE completely to disk, for the user CRED.  If
   WAIT is set, return only after sync is completely finished.  */
error_t
netfs_attempt_sync (struct iouser *cred, struct node *node, int wait)
{
  return ENOTSUP;
}

/* Delete NAME in DIR for USER. */
error_t netfs_attempt_unlink (struct iouser *user, struct node *dir,
			      char *name)
{
  return ENOTSUP;
}

/* Note that in this one call, neither of the specific nodes are locked. */
error_t netfs_attempt_rename (struct iouser *user, struct node *fromdir,
			      char *fromname, struct node *todir,
			      char *toname, int excl)
{
  return ENOTSUP;
}

/* Attempt to create a new directory named NAME in DIR for USER with mode
   MODE.  */
error_t netfs_attempt_mkdir (struct iouser *user, struct node *dir,
			     char *name, mode_t mode)
{
  return ENOTSUP;
}

/* Attempt to remove directory named NAME in DIR for USER. */
error_t netfs_attempt_rmdir (struct iouser *user,
			     struct node *dir, char *name)
{
  return ENOTSUP;
}

/* This should attempt a chmod call for the user specified by CRED on node
   NODE, to change the owner to UID and the group to GID. */
error_t netfs_attempt_chown (struct iouser *cred, struct node *node,
			     uid_t uid, uid_t gid)
{
  return ENOTSUP;
}

/* This should attempt a chauthor call for the user specified by CRED on node
   NODE, to change the author to AUTHOR. */
error_t netfs_attempt_chauthor (struct iouser *cred, struct node *node,
				uid_t author)
{
  return ENOTSUP;
}

/* This should attempt a chmod call for the user specified by CRED on node
   NODE, to change the mode to MODE.  Unlike the normal Unix and Hurd meaning
   of chmod, this function is also used to attempt to change files into other
   types.  If such a transition is attempted which is impossible, then return
   EOPNOTSUPP.  */
error_t netfs_attempt_chmod (struct iouser *cred, struct node *node,
			     mode_t mode)
{
  return ENOTSUP;
}

/* Attempt to turn NODE (user CRED) into a symlink with target NAME. */
error_t netfs_attempt_mksymlink (struct iouser *cred, struct node *node,
				 char *name)
{
  return ENOTSUP;
}

/* Attempt to turn NODE (user CRED) into a device.  TYPE is either S_IFBLK or
   S_IFCHR. */
error_t netfs_attempt_mkdev (struct iouser *cred, struct node *node,
			     mode_t type, dev_t indexes)
{
  return ENOTSUP;
}

/* Attempt to set the passive translator record for FILE to ARGZ (of length
   ARGZLEN) for user CRED. */
error_t netfs_set_translator (struct iouser *cred, struct node *node,
			      char *argz, size_t argzlen)
{
  return ENOTSUP;
}

/* The user may define this function (but should define it together
   with netfs_set_translator).  For locked node NODE with S_IPTRANS
   set in its mode, look up the name of its translator.  Store the
   name into newly malloced storage, and return it in *ARGZ; set
   *ARGZ_LEN to the total length.  */
error_t netfs_get_translator (struct node *node, char **argz,
			      size_t *argz_len)
{
  return ENOTSUP;
}

/* This should attempt a chflags call for the user specified by CRED on node
   NODE, to change the flags to FLAGS. */
error_t netfs_attempt_chflags (struct iouser *cred, struct node *node,
			       int flags)
{
  return ENOTSUP;
}

/* This should attempt to set the size of the file NODE (for user CRED) to
   SIZE bytes long. */
error_t netfs_attempt_set_size (struct iouser *cred, struct node *node,
				off_t size)
{
  return ENOTSUP;
}

/* Create a link in DIR with name NAME to FILE for USER.  Note that neither
   DIR nor FILE are locked.  If EXCL is set, do not delete the target, but
   return EEXIST if NAME is already found in DIR.  */
error_t netfs_attempt_link (struct iouser *user, struct node *dir,
			    struct node *file, char *name, int excl)
{
  return ENOTSUP;
}

/* Attempt to create an anonymous file related to DIR for USER with MODE.
   Set *NODE to the returned file upon success.  No matter what, unlock DIR. */
error_t netfs_attempt_mkfile (struct iouser *user, struct node *dir,
			      mode_t mode, struct node **node)
{
  *node = NULL;
  pthread_mutex_unlock (&dir->lock);
  return ENOTSUP;
}

/* Read the contents of NODE (a symlink), for USER, into BUF. */
error_t netfs_attempt_readlink (struct iouser *user, struct node *node, char *buf)
{
  struct vfs_hooks *hooks = node->nn->fs->hooks;
  error_t err = (hooks->readlink) ? netfs_check_open_permissions (user, node, O_READ, 0) : ENOTSUP;
  if (! err && (node->nn_stat.st_mode & S_IFMT) != S_IFLNK)
    return EINVAL;

  char *content;
  err = hooks->readlink(hooks, node->nn_stat.st_ino, &content);
  if (!err)
    {
      bcopy (content, buf, node->nn_stat.st_size);
      free(content);
    }
  return err;
}

/* Read from the file NODE for user CRED starting at OFFSET and continuing for
   up to *LEN bytes.  Put the data at DATA.  Set *LEN to the amount
   successfully read upon return.  */
error_t netfs_attempt_read (struct iouser *cred, struct node *node,
			    off_t offset, size_t *len, void *data)
{
  return ENOTSUP;
}

/* Write to the file NODE for user CRED starting at OFSET and continuing for up
   to *LEN bytes from DATA.  Set *LEN to the amount seccessfully written upon
   return. */
error_t netfs_attempt_write (struct iouser *cred, struct node *node,
			     off_t offset, size_t *len, void *data)
{
  return ENOTSUP;
}