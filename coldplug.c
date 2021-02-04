/*
 * mediad -- daemon to automount removable media
 *
 * Copyright (c) 2006-2021 by Roman Hodek <roman@hodek.net>
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
#include <dirent.h>
#include <errno.h>
#include <unistd.h>
#include <limits.h>
#include <fcntl.h>
#include "mediad.h"


static int is_media_dev(const char *devpath)
{
	FILE *f;
	char path_removable[PATH_MAX];
	int removable;

	/* is the device marked as removable? */
	snprintf(path_removable, sizeof(path_removable), "%.*s/removable",
			 PATH_MAX-12, devpath);
	if ((f = fopen(path_removable, "r"))) {
		if (fscanf(f, "%d", &removable) != 1)
			removable = 0;
		fclose(f);
	}
	else
		removable = 0;
	if (removable)
		return 1;

	if (strstr(devpath, "/usb"))
		return 1;
	if (strstr(devpath, "/ieee1394"))
		return 1;

	return 0;
}

static void add_mount_with_partitions(const char *name, const char *devpath)
{
	char devname[PATH_MAX];
	char subdevpath[PATH_MAX];
	DIR *dir;
	struct dirent *de;
	size_t syslen = strlen("/sys");
	size_t namelen = strlen(name);
	
	snprintf(devname, sizeof(devname), "/dev/%s", name);
	add_mount_with_devpath(devname, devpath+syslen);

	/* also add all partitions */
	if (!(dir = opendir(devpath)))
		return;
	while((de = readdir(dir))) {
		if (de->d_type == DT_DIR &&
			strncmp(de->d_name, name, namelen) == 0) {
			
			snprintf(devname, sizeof(devname), "/dev/%s", de->d_name);
			snprintf(subdevpath, sizeof(subdevpath), "%.*s/%s",
					 (int)(PATH_MAX-strlen(de->d_name)-2),
					 devpath, de->d_name);
			add_mount_with_devpath(devname, subdevpath+syslen);
		}
	}
	closedir(dir);
}

/* replay events of already existing devices (at start time) */
void coldplug(void)
{
	const char *blockdevs = "/sys/block";
	DIR *dir;
	struct dirent *de;
	ssize_t n;
	char path[PATH_MAX];
	char linkto[PATH_MAX];

	/* iterate over all block devices */
	if (!(dir = opendir(blockdevs)))
		return;
	while((de = readdir(dir))) {
		if (de->d_type != DT_LNK)
			continue;
		/* read the link (to devpath) */
		n = readlinkat(dirfd(dir), de->d_name, linkto, sizeof(linkto));
		if (n < 0 || n >= sizeof(linkto))
			continue;
		linkto[n] = '\0';
		
		if (strncmp(linkto, "../", 3) == 0)
			snprintf(path, sizeof(path), "/sys/%.*s",
					 PATH_MAX-6, linkto+3);
		else
			snprintf(path, sizeof(path), "%s/%.*s",
					 blockdevs, PATH_MAX-12, linkto);
debug("coldplug: found block/%s -> %s", de->d_name, path);

		if (is_media_dev(path))
			add_mount_with_partitions(de->d_name, path);
	}
	closedir(dir);
}
