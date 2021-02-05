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
#include <libudev.h>
#include "mediad.h"


/* replay events of already existing devices (at start time) */
void coldplug(void)
{
	struct udev_enumerate *d_enum, *p_enum;
	struct udev_list_entry *d_ent, *p_ent;

	if (!(d_enum = udev_enumerate_new(udev))) {
		error("cannot create udev enumerator");
		return;
	}
	udev_enumerate_add_match_subsystem(d_enum, "block");
#if 0
	/* The two busses seem to be OR-ed, which the sysattr AND-ed, so would
	 * need two runs, yielding partially same results? For now it seems
	 * sufficient to check the removable flag. */
	udev_enumerate_add_match_property(d_enum, "ID_BUS", "usb");
	udev_enumerate_add_match_property(d_enum, "ID_BUS", "ieee1394");
#endif
	udev_enumerate_add_match_sysattr(d_enum, "removable", "1");
	udev_enumerate_scan_devices(d_enum);

	udev_list_entry_foreach(d_ent, udev_enumerate_get_list_entry(d_enum)) {
		struct udev_device *dev =
			udev_device_new_from_syspath
			(udev, udev_list_entry_get_name(d_ent));
		if (!dev)
			continue;
		if (!(p_enum = udev_enumerate_new(udev))) {
			error("cannot create udev enumerator");
			continue;
		}
		udev_enumerate_add_match_parent(p_enum, dev);
		udev_enumerate_scan_devices(p_enum);
		udev_list_entry_foreach(p_ent, udev_enumerate_get_list_entry(p_enum)) {
				struct udev_device *part =
					udev_device_new_from_syspath
					(udev, udev_list_entry_get_name(p_ent));
				if (!part)
					continue;
				char devname[PATH_MAX];
				snprintf(devname, sizeof(devname), "/dev/%s",
						 udev_device_get_sysname(part));
				add_mount_with_devpath(devname,
									   udev_device_get_devpath(part));
				udev_device_unref(part);
		}
		udev_enumerate_unref(p_enum);
		udev_device_unref(dev);
	}
	udev_enumerate_unref(d_enum);
}
