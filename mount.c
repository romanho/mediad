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
#include <stdarg.h>
#include <alloca.h>
#include <pthread.h>
#include <ctype.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <limits.h>
#include <mntent.h>
#include <syslog.h>
#include <dirent.h>
#include <sys/mount.h>
#include <sys/wait.h>
#include "mediad.h"


typedef struct __mhelper {
	struct __mhelper *next;
	char *fstype;
	char *binary;
} mhelper_t;

static mhelper_t *mhelpers = NULL;
static int mhelpers_inited = 0;
static pthread_mutex_t mhelpers_lock = PTHREAD_MUTEX_INITIALIZER;


typedef struct __fsrepl {
	struct __fsrepl *next;
	char *from;
	char *to;
} fsrepl_t;

static fsrepl_t *fsreplace = NULL;
static pthread_mutex_t fsrepl_lock = PTHREAD_MUTEX_INITIALIZER;


static void init_mount_helpers(void)
{
	DIR *d;
	struct dirent *de;
	char **dir;
	static char *sbin_dirs[] =
		{ "/sbin", "/usr/sbin", "/usr/local/sbin", NULL };
	
	if (mhelpers_inited)
		return;

	pthread_mutex_lock(&mhelpers_lock);
	for(dir = sbin_dirs; *dir; ++dir) {
		if (!(d = opendir(*dir)))
			continue;
		while((de = readdir(d))) {
			if (strprefix(de->d_name, "mount.")) {
				mhelper_t *h = xmalloc(sizeof(mhelper_t));
				h->fstype = xstrdup(de->d_name+6);
				h->binary = xmalloc(strlen(*dir)+1+strlen(de->d_name)+1);
				strcpy(h->binary, *dir);
				strcat(h->binary, "/");
				strcat(h->binary, de->d_name);
				h->next = mhelpers;
				mhelpers = h;
				debug("found mount helper %s for fstype %s", h->binary, h->fstype);
			}
		}
		closedir(d);
	}
	mhelpers_inited = 1;
	pthread_mutex_unlock(&mhelpers_lock);
}

static const char *find_mount_helper(const char *type)
{
	mhelper_t *h;

	init_mount_helpers();
	
	pthread_mutex_lock(&mhelpers_lock);
	for(h = mhelpers; h; h = h->next) {
		if (strcmp(h->fstype, type) == 0) {
			pthread_mutex_unlock(&mhelpers_lock);
			return h->binary;
		}
	}
	pthread_mutex_unlock(&mhelpers_lock);
	return NULL;
}


void add_fstype_replace(const char *from, const char *to)
{
	fsrepl_t *h = xmalloc(sizeof(fsrepl_t));

	h->from = xstrdup(from);
	h->to = xstrdup(to);
	pthread_mutex_lock(&fsrepl_lock);
	h->next = fsreplace;
	fsreplace = h;
	pthread_mutex_unlock(&fsrepl_lock);
}

static char *find_fstype_replace(const char *type)
{
	fsrepl_t *h;

	pthread_mutex_lock(&fsrepl_lock);
	for(h = fsreplace; h; h = h->next) {
		if (strcmp(h->from, type) == 0) {
			pthread_mutex_unlock(&fsrepl_lock);
			return h->to;
		}
	}
	pthread_mutex_unlock(&fsrepl_lock);
	return NULL;
}

void purge_fstype_replace(void)
{
	fsrepl_t **oo;

	pthread_mutex_lock(&fsrepl_lock);
	oo = &fsreplace;
	while(*oo) {
		fsrepl_t *o = *oo;

		*oo = o->next;
		free(o->from);
		free(o->to);
		free(o);
	}
	pthread_mutex_unlock(&fsrepl_lock);
}


int call_mount(const char *dev, const char *path,
			   const char *type, const char *options)
{
	const char *repltype;
	const char *helper;
	int serrno;
	int iopts = 0;
	const char *sopts;

	if ((repltype = find_fstype_replace(type))) {
		debug("replacing fstype %s by %s", type, repltype);
		type = repltype;
	}

	if ((helper = find_mount_helper(type))) {
		pid_t pid;

		debug("calling mount helper %s %s %s -o %s", helper, dev, path, options);
		if ((pid = fork()) < 0) {
			error("fork: %s", strerror(errno));
			errno = -ENOENT;
			return -1;
		}
		if (pid == 0) {
			int i = 0;
			while(used_sigs[i])
				signal(used_sigs[i++], SIG_DFL);
			
			close(0);
			open("/dev/null", O_RDONLY);
			close(1);
			open("/dev/null", O_WRONLY);
			close(2);
			open("/dev/null", O_RDWR);
			execl(helper, helper, dev, path,
				  options ? "-o" : NULL, options, NULL);
			exit(127);
		}
		else {
			int status = 0;
			waitpid(pid, &status, 0);
			debug("helper returned status %d", status);
			if (status == 0)
				return 0;
			errno = EINVAL;
			return -1;
		}
	}
	else {
		parse_mount_options(options, &iopts, &sopts);
		if (mount(dev, path, type, iopts, sopts) == 0) {
			free((char*)sopts);
			add_mtab(dev, path, type, options);
			return 0;
		}
		if (errno == EROFS) {
			iopts |= MS_RDONLY;
			if (mount(dev, path, type, iopts, sopts) == 0) {
				free((char*)sopts);
				add_mtab(dev, path, type, options);
				return 1;
			}
		}
		serrno = errno;
		free((char*)sopts);
		errno = serrno;
		return -1;
	}
}
