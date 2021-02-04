/*
 * mediad -- daemon to automount removable media
 *
 * Copyright (c) 2006 by Roman Hodek <roman@hodek.net>
 * 
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2.1 of the License, or (at your option)
 * any later version.
 * 
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Library General Public
 * License for more details.
 * 
 * You should have received a copy of the GNU Library General Public License
 * along with this library; if not, write to the Free Software Foundation,
 * Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307  USA.
 *
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <linux/cdrom.h>
#include <linux/fd.h>
#include "mediad.h"

int no_medium_errno(void)
{
	return errno == ENOMEDIUM || errno == ENXIO ||
		   errno == ENODEV || errno == EIO;
}

int check_medium(const char *dev)
{
	int fd;

	if ((fd = open(dev, O_RDONLY)) < 0)
		return !no_medium_errno();
	close(fd);
	return 1;
}


/* The kernel checks for media changes and notices them reliably but,
 * unfortunately, the results of these checks are easily visible from
 * userland :(
 * Only for CD-ROMs, there's a dedicated ioctl for this purpose. For (PC)
 * floppies, the status data contain a generation counter that's incremented
 * for each disk change, which is not as direct but works fine, too.
 * However, I haven't found a good method for SCSI disks :( On the other hand,
 * those devices are usually sufficiently fast so that running vol_id too
 * often doesn't hurt that much.
 */

static int check_changed_cdrom(int fd, mnt_t *m)
{
	return ioctl(fd, CDROM_MEDIA_CHANGED, 0);
}

static int check_changed_floppy(int fd, mnt_t *m)
{
	struct floppy_drive_struct fds;
	
	if (ioctl(fd, FDGETDRVSTAT, &fds))
		return -1;
	if (fds.generation != m->check_change_param) {
		m->check_change_param = fds.generation;
		return 1;
	}
	return 0;
}

static int check_changed(mnt_t *m)
{
	int fd, rv;

	if ((fd = open(m->dev, O_RDONLY)) < 0) {
		if (no_medium_errno())
			set_no_medium_present(m);
		debug("check_changed(%s): open failed: %s", m->dev, strerror(errno));
		return 0;
	}
	switch(m->check_change_strategy) {
	  case CCS_UNKNOWN:
		if ((rv = check_changed_cdrom(fd, m)) >= 0) {
			m->check_change_strategy = CCS_CDROM;
			break;
		}
		if ((rv = check_changed_floppy(fd, m)) >= 0) {
			m->check_change_strategy = CCS_FLOPPY;
			break;
		}
		m->check_change_strategy = CCS_NONE;
		/* fall through */
		
	  case CCS_NONE:
	  default:
		rv = 1;
		break;
		
	  case CCS_CDROM:
		rv = check_changed_cdrom(fd, m);
		break;

	  case CCS_FLOPPY:
		rv = check_changed_floppy(fd, m);
		break;
	}
	close(fd);
	if (rv < 0)
		rv = 1;

	return rv;
}

static void remake_fsspec_aliases(mnt_t *m)
{
	mark_aliases(m, AF_FSSPEC, AF_FSSPEC, AF_OLD);

	get_dev_infos(m);
	if (m->type) {
		mnt_add_label_alias(m, AF_OLD);
		mnt_add_uuid_alias(m, AF_OLD);
		match_aliases(m, 1, AF_OLD);
	}
	
	mk_aliases(m, WAT_FSSPEC);
	mnt_free_aliases(m, AF_OLD, AF_OLD);
}

void check_medium_change(mnt_t *m)
{
	mnt_t *mm = m;
	
	/* For children (partitions) we don't need to check for medium
	 * changes; if such a change happens, the partitions receive remove
	 * events and eventually add events after re-reading the partition
	 * table. So children trigger a medium check at the parent, which in turn
	 * may remove them. */
	if (m->parent) {
		mm = m->parent;
		pthread_mutex_lock(&mm->lock);
	}
	
	if (!mm->medium_present) {
		if ((mm->medium_present = check_medium(mm->dev))) {
			debug("%s: medium now present, assuming changed", mm->dev);
			mm->medium_changed = 1;
		}
		else
			debug("%s: still no medium", mm->dev);
	}
	else {
		if (!mm->medium_changed) {
			if ((mm->medium_changed = check_changed(mm)))
				debug("%s: change detected", mm->dev);
			else
				debug("%s: no change detected", mm->dev);
		}
		else
			debug("%s: was already marked as changed", mm->dev);
	}
	
	if (mm->medium_present && mm->medium_changed) {
		remake_fsspec_aliases(mm);
		mm->medium_changed = 0;
	}

	if (m != mm)
		pthread_mutex_unlock(&mm->lock);
}

void set_no_medium_present(mnt_t *m)
{
	mnt_t *mm = m;

	if (m->parent) {
		pthread_mutex_lock(&m->parent->lock);
		mm = m->parent;
	}

	debug("%s: no medium anymore", mm->dev);
	mm->medium_present = 0;
	rm_aliases(mm, WAT_FSSPEC);
	mnt_free_aliases(mm, AF_FSSPEC, AF_FSSPEC);

	if (m->parent)
		pthread_mutex_unlock(&m->parent->lock);
}
