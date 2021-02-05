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
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <syslog.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <mntent.h>
#include <ftw.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/wait.h>
#include <sys/sysmacros.h>
#include <sys/utsname.h>
#include <netinet/in.h>
#include <libudev.h>
#include <linux/version.h>
#include "mediad.h"


void logit(int pri, const char *fmt, ...)
{
	va_list argp;

	va_start(argp, fmt);
	vsyslog(pri, fmt, argp);
	va_end(argp);
}

void *xmalloc(size_t sz)
{
	void *p = malloc(sz);
	if (!p)
		fatal("out of memory");
	return p;
}

void *xrealloc(void *p, size_t sz)
{
	p = realloc(p, sz);
	if (!p)
		fatal("out of memory");
	return p;
}

char *xstrdup(const char *str)
{
	char *p;

	if (!str)
		return NULL;
	p = strdup(str);
	if (!p)
		fatal("out of memory");
	return p;
}

void xfree(const char **p)
{
	if (*p) {
		free((char*)*p);
		*p = NULL;
	}
}


char *mkpath(char *buf, const char *add)
{
	char *p;
	
	strcpy(buf, autodir);
	strcat(buf, "/");
	p = buf+strlen(buf);
	if (add)
		strcpy(p, add);
	return p;
}

#define Iget(name,field)											\
	do {															\
		int __l = strlen(name);										\
		if (strncmp(line, name, __l) == 0 && line[__l] == '=') {	\
			xfree(&m->field);										\
			m->field = xstrdup(line+__l+1);							\
			debug("found %s = '%s'", #field, m->field);				\
			return;													\
		}															\
	} while(0)

void parse_id(mnt_t *m, const char *line)
{
	Iget("DEVPATH",				devpath);
	Iget("ID_VENDOR",			vendor);
	Iget("ID_MODEL",			model);
	Iget("ID_SERIAL",			serial);
	Iget("ID_FS_TYPE",			type);
	Iget("ID_FS_UUID",			uuid);
	Iget("ID_FS_LABEL",			label);
}

void replace_untrusted_chars(char *p)
{
	for(; *p; ++p) {
		if (*p < ' ' || strchr("!\"&'()*;<>[\\]^`{|}~", *p))
			*p = '_';
	}
}

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
		if (pnam && pval && strcmp(pnam, "DEVPATH") != 0) {
			char buf[strlen(pnam)+strlen(pval)+2];
			sprintf(buf, "%s=%s", pnam, pval);
			replace_untrusted_chars(buf);
			parse_id(m, buf);
		}
		list_entry = udev_list_entry_get_next(list_entry);
	}

	udev_device_unref(dev);
}

static __thread unsigned find_maj, find_min;
static __thread char *found_path;

static int find_dev(const char *name, const struct stat *st,
					int flag, struct FTW *ft)
{
	FILE *f;
	unsigned ma, mi;
	
	if (flag != FTW_F || strcmp(name+ft->base, "dev") != 0)
		return 0;
	if (!(f = fopen(name, "r"))) {
		error("%s: %s", name, strerror(errno));
		return 0;
	}
	if (fscanf(f, "%u:%u", &ma, &mi) != 2) {
		error("%s: bad contents", name);
		return 0;
	}
	fclose(f);
	
	if (ma == find_maj && mi == find_min) {
		char path[PATH_MAX];
		getcwd(path, sizeof(path));
		/* strdup the dir name where the dev file was found, without the /sys
		 * prefix to be consistent with what udev passes to us */
		found_path = xmalloc(strlen(path)-4+1);
		strcpy(found_path, path+4);
		return 1;
	}
	return 0;
}

void find_devpath(mnt_t *m)
{
	struct stat st;

	if (stat(m->dev, &st)) {
		error("stat(%s): %s", m->dev, strerror(errno));
		return;
	}
	find_maj = major(st.st_rdev);
	find_min = minor(st.st_rdev);

	found_path = NULL;
	nftw("/sys/block", find_dev, 10, FTW_MOUNT|FTW_CHDIR);

	m->devpath = found_path;
	debug("found devpath=%s for %s by searching /sys/block",
		  m->devpath ? m->devpath : "NONE", m->dev);
}


void mk_dir(mnt_t *m)
{
	char path[PATH_MAX];
	
	mkpath(path, m->dir);
	if (mkdir(path, 0755) && errno != EEXIST)
		error("mkdir(%s): %s", path, strerror(errno));
}

void rm_dir(mnt_t *m)
{
	char path[PATH_MAX];
	
	mkpath(path, m->dir);
	if (rmdir(path))
		warning("rmdir(%s): %s", path, strerror(errno));
}

void send_num(int fd, unsigned num)
{
	union {
		u_int16_t i;
		unsigned char c[2];
	} u;

	if (num > USHRT_MAX)
		fatal("Number too large (%u)", num);
	u.i = htons(num);
	write(fd, u.c, 2);
}

void send_str(int fd, const char *str)
{
	int len = strlen(str)+1;

	send_num(fd, len);
	write(fd, str, len);
}

unsigned recv_num(int fd)
{
	union {
		u_int16_t i;
		unsigned char c[2];
	} u;
	
	if (read(fd, u.c, 2) != 2)
		fatal("read from cmd socket: %s", strerror(errno));
	return ntohs(u.i);
}

void recv_str(int fd, char **p)
{
	int len = recv_num(fd);

	*p = xmalloc(len);
	if (read(fd, (char*)*p, len) != len)
		fatal("read from cmd socket: %s", strerror(errno));
}

unsigned int linux_version_code(void)
{
	struct utsname my_utsname;
	unsigned int p, q, r;
	char *tmp, *save;

	if (uname(&my_utsname))
		return 0;

	p = q = r = 0;

	tmp = strtok_r(my_utsname.release, ".", &save);
	if (!tmp)
		return 0;
	p = (unsigned int ) atoi(tmp);

	tmp = strtok_r(NULL, ".", &save);
	if (!tmp)
		return KERNEL_VERSION(p, 0, 0);
	q = (unsigned int) atoi(tmp);

	tmp = strtok_r(NULL, ".", &save);
	if (!tmp)
		return KERNEL_VERSION(p, q, 0);
	r = (unsigned int) atoi(tmp);

	return KERNEL_VERSION(p, q, r);
}
