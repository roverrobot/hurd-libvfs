/* io_map for libremotefs

   Copyright (C) 1995,96,97,99,2000,02,20
   Written by Junling Ma <junlingm@gmail.com>

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

#include "libvfs/vfs.h"

#include <hurd/netfs.h>
#include <hurd/pager.h>
#include <stdio.h>
#include <fcntl.h>
#include <sys/mman.h>

/* ---------------------------------------------------------------- */
/* Pager library callbacks; see <hurd/pager.h> for more info.  */

void
pager_dropweak (struct user_pager_info *upi __attribute__ ((unused)))
{
}

/* For pager PAGER, read one page from offset PAGE.  Set *BUF to be the
   address of the page, and set *WRITE_LOCK if the page must be provided
   read-only.  The only permissible error returns are EIO, EDQUOT, and
   ENOSPC. */
error_t
pager_read_page (struct user_pager_info *upi,
		 vm_offset_t page, vm_address_t *buf, int *writelock)
{
  error_t err;
  size_t want = vm_page_size;	/* bytes we want to read */
  struct netnode *nn = (struct netnode *)upi;

  if (page + want > nn->node->nn_stat.st_size)
    /* Read a partial page if necessary to avoid reading off the end.  */
    want = nn->node->nn_stat.st_size - page;

  *buf = (vm_address_t) mmap(0, vm_page_size, PROT_READ|PROT_WRITE, MAP_ANON, 0, 0);
  if (*buf == 0)
    return ENOMEM;

  struct vfs_hooks *hooks = nn->fs->hooks;
  size_t read = want;		/* bytes actually read */
  err = hooks->read (nn->file, page, (void *)*buf, &read);

  if (!err && (want < vm_page_size))
    /* Zero anything we didn't read.  Allocation only happens in page-size
       multiples, so we know we can write there.  */
    memset ((char *)*buf + want, '\0', vm_page_size - want);

  *writelock = 0;

  return (err || read < want) ? EIO : ESUCCESS;
}

/* For pager PAGER, synchronously write one page from BUF to offset PAGE.  In
   addition, vm_deallocate (or equivalent) BUF.  The only permissible error
   returns are EIO, EDQUOT, and ENOSPC. */
error_t
pager_write_page (struct user_pager_info *upi,
		  vm_offset_t page, vm_address_t buf)
{
  error_t err;
  size_t want = vm_page_size;
  struct netnode *nn = (struct netnode *)upi;

  if (page + want > nn->node->nn_stat.st_size)
    /* Write a partial page if necessary to avoid reading off the end.  */
    want = nn->node->nn_stat.st_size - page;

  struct vfs_hooks *hooks = nn->fs->hooks;
  size_t written = want;
  err = hooks->write (nn->file, page, (char *)buf, &want);
  
  munmap ((caddr_t) buf, vm_page_size);
  return (err || written < want) ? EIO : ESUCCESS;
}

/* A page should be made writable. */
error_t
pager_unlock_page (struct user_pager_info *upi, vm_offset_t address)
{
  return 0;
}

void
pager_notify_evict (struct user_pager_info *pager,
		    vm_offset_t page)
{
  assert_backtrace (!"unrequested notification on eviction");
}

/* The user must define this function.  It should report back (in
   *OFFSET and *SIZE the minimum valid address the pager will accept
   and the size of the object.   */
error_t
pager_report_extent (struct user_pager_info *upi,
		    vm_address_t *offset, vm_size_t *size)
{
  struct netnode *nn = (struct netnode *)upi;
  *offset = 0;
  *size = nn->node->nn_stat.st_size;
  return ESUCCESS;
}

/* This is called when a pager is being deallocated after all extant send
   rights have been destroyed.  */
void
pager_clear_user_data (struct user_pager_info *upi)
{
  struct netnode *nn = (struct netnode *)upi;
  struct vfs *fs = nn->fs;
  pthread_spin_lock (&fs->pager_lock);
  nn->pager = 0;
  netfs_nrele(nn->node);
  pthread_spin_unlock (&fs->pager_lock);
}

/* Returns in MEMOBJ the port for a memory object backed by the storage on
   DEV.  Returns 0 or the error code if an error occurred.  */
static error_t
get_memory_object (struct netnode *nn, vm_prot_t prot, memory_object_t *memobj)
{
  int created = 0;
  error_t err;
  pthread_spin_lock (&nn->fs->pager_lock);

  if (nn->pager == NULL)
    {
      nn->pager = pager_create ((struct user_pager_info *)nn, nn->fs->pager_port_bucket,
          1, MEMORY_OBJECT_COPY_DELAY, 0);
      /* Start libpagers worker threads.  */
      if (nn->pager == NULL)
        {
          pthread_spin_unlock (&nn->fs->pager_lock);
          return errno;
        }
      
      netfs_nref(nn->node);
      created = 1;
    }

  *memobj = pager_get_port (nn->pager);
  if (*memobj == MACH_PORT_NULL)
    /* Pager is currently being destroyed, try again.  */
    {
      nn->pager = NULL;
      pthread_spin_unlock (&nn->fs->pager_lock);
      return get_memory_object (nn, prot, memobj);
    }

	err = mach_port_insert_right (mach_task_self (), *memobj, *memobj, MACH_MSG_TYPE_MAKE_SEND);

  if (created)
    ports_port_deref (nn->pager);
  pthread_spin_unlock (&nn->fs->pager_lock);
  return err;
}

/* Return objects mapping the data underlying this memory object.  If
   the object can be read then memobjrd will be provided; if the
   object can be written then memobjwr will be provided.  For objects
   where read data and write data are the same, these objects will be
   equal, otherwise they will be disjoint.  Servers are permitted to
   implement io_map but not io_map_cntl.  Some objects do not provide
   mapping; they will set none of the ports and return an error.  Such
   objects can still be accessed by io_read and io_write.  */
error_t
netfs_S_io_map (struct protid *user, 
		mach_port_t *rdobj, mach_msg_type_name_t *rdobjtype,
		mach_port_t *wrobj, mach_msg_type_name_t *wrobjtype)
{
  struct vfs_hooks *hooks = user->po->np->nn->fs->hooks;
  int flags = user->po->openstat;
  if (! user->user)
    return EOPNOTSUPP;
  
  if (! (flags & (O_READ|O_WRITE)))
    return EBADF;
  
  vm_prot_t prot = 0;
  if (flags & O_READ)
    {
      if (!hooks->read)
        return EOPNOTSUPP;
      prot |= VM_PROT_READ;
    }
  if (flags & O_WRITE)
    {
      if (!hooks->write)
        return EOPNOTSUPP;
      prot |= VM_PROT_WRITE;
    }
  
  struct node * node = user->po->np;
  pthread_mutex_lock(&user->po->np->lock);
  error_t err = ESUCCESS;
  if (node->nn->file == NULL)
    err = hooks->open(hooks, node->nn_stat.st_ino, flags, 0, &node->nn->file);
  mach_port_t memobj = MACH_PORT_NULL;
  if (!err)
    err = get_memory_object (user->po->np->nn, prot, &memobj);
  pthread_mutex_unlock(&user->po->np->lock);

  if (!err)
    {
      *rdobj = (flags & O_READ) ? memobj : MACH_PORT_NULL;
      *wrobj = (flags & O_WRITE) ? memobj : MACH_PORT_NULL;

      if ((flags & O_READ) && (flags & O_WRITE) && memobj != MACH_PORT_NULL)
        mach_port_mod_refs (mach_task_self (), memobj, MACH_PORT_RIGHT_SEND, 1);
    }

  *rdobjtype = *wrobjtype = MACH_MSG_TYPE_MOVE_SEND;
  return err;
}
