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
#define _GNU_SOURCE
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


void get_dev_infos(mnt_t *m)
{
	struct udev_device *dev;
	struct udev_list_entry *list_entry;
	char sdevpath[strlen(m->devpath)+5];
	sprintf(sdevpath, "/sys/%s", m->devpath);
	
	if (!(dev = udev_device_new_from_syspath(udev, sdevpath))) {
		error("%s: failed to get udev object", m->dev);
		return;
	}

	list_entry = udev_device_get_properties_list_entry(dev);
	while(list_entry) {
		const char *pnam = udev_list_entry_get_name(list_entry);
		const char *pval = udev_list_entry_get_value(list_entry);
		if (pnam && pval && !streq(pnam, "DEVPATH")) {
			char buf[strlen(pnam)+strlen(pval)+2];
			sprintf(buf, "%s=%s", pnam, pval);
			replace_untrusted_chars(buf);
			parse_id(m, buf);
		}
		list_entry = udev_list_entry_get_next(list_entry);
	}

	udev_device_unref(dev);
}

void find_devpath(mnt_t *m)
{
	const char *devname = m->dev, *p;
	struct udev_device *dev;

	if (m->devpath)
		return;
	if ((p = strprefix(devname, "/dev/")))
		devname = p;

	if (!(dev = udev_device_new_from_subsystem_sysname(udev, "block", devname))) {
		error("%s: failed to get udev object", m->dev);
		return;
	}
	m->devpath = xstrdup(udev_device_get_devpath(dev));

	udev_device_unref(dev);
	debug("found devpath=%s for %s",
		  m->devpath ? m->devpath : "NONE", m->dev);
}

int find_by_property(const char *propname, const char *propval,
					 char *outname, size_t outsize)
{
	struct udev_enumerate *d_enum;
	struct udev_list_entry *d_ent;

	*outname = '\0';
	
	if (streq(propname, "LABEL"))
		propname = "ID_FS_LABEL";
	else if (streq(propname, "UUID"))
		propname = "ID_FS_UUID";
	else {
		warning("find_by_property: unhandled property '%s' (from /etc/fstab)",
				propname);
		return 0;
	}
	
	if (!(d_enum = udev_enumerate_new(udev))) {
		error("cannot create udev enumerator");
		return 0;
	}
	udev_enumerate_add_match_subsystem(d_enum, "block");
	udev_enumerate_add_match_property(d_enum, propname, propval);
	udev_enumerate_scan_devices(d_enum);

	udev_list_entry_foreach(d_ent, udev_enumerate_get_list_entry(d_enum)) {
		struct udev_device *dev =
			udev_device_new_from_syspath
			(udev, udev_list_entry_get_name(d_ent));
		if (!dev)
			continue;
		if (!outname[0])
			snprintf(outname, outsize, "/dev/%s", udev_device_get_sysname(dev));
		else	
			warning("ambigous %s=%s (%s and %s are matching)",
					propname, propval, outname,
					udev_device_get_sysname(dev));
	}
	udev_enumerate_unref(d_enum);

	if (!outname[0]) {
		warning("no device found for %s=%s", propname, propval);
		return 0;
	}
	return 1;
}

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
