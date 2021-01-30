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
#include <stdarg.h>
#include <ctype.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/mount.h>
#include "mediad.h"


fsoptions_t *fsoptions = NULL;
static pthread_mutex_t fsoptions_lock = PTHREAD_MUTEX_INITIALIZER;


void add_fsoptions(mcond_t *cond, const char *opts)
{
	fsoptions_t *o = xmalloc(sizeof(fsoptions_t)), **oo;

	o->cond = cond;
	o->prio = mcond_prio(cond);
	o->options = xstrdup(opts);

	pthread_mutex_lock(&fsoptions_lock);
	/* keep sorted by prio, append at end within same level */
	for(oo = &fsoptions; *oo; oo = &(*oo)->next)
		if ((*oo)->prio > o->prio)
			break;
	o->next = *oo;
	*oo = o;
	pthread_mutex_unlock(&fsoptions_lock);
}

const char *find_fsoptions(mnt_t *m)
{
	fsoptions_t *o;

	pthread_mutex_lock(&fsoptions_lock);
	for(o = fsoptions; o; o = o->next) {
		if (match_mcond(o->cond, m, NULL)) {
			const char *opt = o->options;
			pthread_mutex_unlock(&fsoptions_lock);
			return opt;
		}
	}
	pthread_mutex_unlock(&fsoptions_lock);
	return NULL;
}

void purge_fsoptions(void)
{
	fsoptions_t **oo;

	pthread_mutex_lock(&fsoptions_lock);
	oo = &fsoptions;
	while(*oo) {
		fsoptions_t *o = *oo;
		if (o->cond && o->cond->what == MWH_MTABDEVNAME)
			oo = &(*oo)->next;
		else {
			*oo = o->next;
			free_mcond(o->cond);
			free((char*)o->options);
			free(o);
		}
	}
	pthread_mutex_unlock(&fsoptions_lock);
}

void parse_mount_options(const char *_opts, int *iopts, const char **sopts)
{
	char opts[strlen(_opts)+1];
	char _sopts[strlen(_opts)+1];
	char *p;

	*iopts = 0;
	/* strtok() writes to the string, so make a writeable copy */
	strcpy(opts, _opts);
	*_sopts = '\0';
	for(p = strtok(opts, ","); p; p = strtok(NULL, ",")) {
		if (strcmp(p, "ro") == 0)
			*iopts |= MS_RDONLY;
		else if (strcmp(p, "rw") == 0)
			*iopts &= ~MS_RDONLY;
		else if (strcmp(p, "nosuid") == 0)
			*iopts |= MS_NOSUID;
		else if (strcmp(p, "suid") == 0)
			*iopts &= ~MS_NOSUID;
		else if (strcmp(p, "nodev") == 0)
			*iopts |= MS_NODEV;
		else if (strcmp(p, "dev") == 0)
			*iopts &= ~MS_NODEV;
		else if (strcmp(p, "noexec") == 0)
			*iopts |= MS_NOEXEC;
		else if (strcmp(p, "exec") == 0)
			*iopts &= ~MS_NOEXEC;
		else if (strcmp(p, "sync") == 0)
			*iopts |= MS_SYNCHRONOUS;
		else if (strcmp(p, "async") == 0)
			*iopts &= ~MS_SYNCHRONOUS;
		else if (strcmp(p, "mand") == 0)
			*iopts |= MS_MANDLOCK;
		else if (strcmp(p, "nomand") == 0)
			*iopts &= ~MS_MANDLOCK;
		else if (strcmp(p, "noatime") == 0)
			*iopts |= MS_NOATIME;
		else if (strcmp(p, "atime") == 0)
			*iopts &= ~MS_NOATIME;
		else if (strcmp(p, "nodiratime") == 0)
			*iopts |= MS_NODIRATIME;
		else if (strcmp(p, "diratime") == 0)
			*iopts &= ~MS_NODIRATIME;
		else if (strcmp(p, "auto") == 0 || strcmp(p, "noauto") == 0 ||
				 strcmp(p, "user") == 0 || strcmp(p, "nouser") == 0 ||
				 strcmp(p, "users") == 0 || strcmp(p, "nousers") == 0 ||
				 strncmp(p, "fs=", 3) == 0)
			/* omit */ ;
		else {
			/* if not a predefined option, append to string options and pass
			 * through later */
			if (*_sopts)
				strcat(_sopts, ",");
			strcat(_sopts, p);
		}
	}
	*sopts = xstrdup(_sopts);
}
