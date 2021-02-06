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
mntoptions_t *mntoptions = NULL;
static pthread_mutex_t fsoptions_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t mntoptions_lock = PTHREAD_MUTEX_INITIALIZER;


static char *filter_options(const char *_opts)
{
	char opts[strlen(_opts)+1];
	char sopts[strlen(_opts)+1];
	char *p;

	/* strtok() writes to the string, so make a writeable copy */
	strcpy(opts, _opts);
	sopts[0] = '\0';
	for(p = strtok(opts, ","); p; p = strtok(NULL, ",")) {
		/* filter out options that have meaning only in /etc/fstab */
		if (!streq(p, "auto") && !streq(p, "noauto") &&
			!streq(p, "user") && !streq(p, "nouser") &&
			!streq(p, "users") && !streq(p, "nousers") &&
			!strprefix(p, "fs=")) {
			if (sopts[0])
				strcat(sopts, ",");
			strcat(sopts, p);
		}
	}
	return xstrdup(sopts);
}

void add_fsoptions(mcond_t *cond, const char *opts)
{
	fsoptions_t *o = xmalloc(sizeof(fsoptions_t)), **oo;

	o->cond = cond;
	o->prio = mcond_prio(cond);
	o->options = filter_options(opts);

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
		if (streq(p, "ro"))
			*iopts |= MS_RDONLY;
		else if (streq(p, "rw"))
			*iopts &= ~MS_RDONLY;
		else if (streq(p, "nosuid"))
			*iopts |= MS_NOSUID;
		else if (streq(p, "suid"))
			*iopts &= ~MS_NOSUID;
		else if (streq(p, "nodev"))
			*iopts |= MS_NODEV;
		else if (streq(p, "dev"))
			*iopts &= ~MS_NODEV;
		else if (streq(p, "noexec"))
			*iopts |= MS_NOEXEC;
		else if (streq(p, "exec"))
			*iopts &= ~MS_NOEXEC;
		else if (streq(p, "sync"))
			*iopts |= MS_SYNCHRONOUS;
		else if (streq(p, "async"))
			*iopts &= ~MS_SYNCHRONOUS;
		else if (streq(p, "mand"))
			*iopts |= MS_MANDLOCK;
		else if (streq(p, "nomand"))
			*iopts &= ~MS_MANDLOCK;
		else if (streq(p, "noatime"))
			*iopts |= MS_NOATIME;
		else if (streq(p, "atime"))
			*iopts &= ~MS_NOATIME;
		else if (streq(p, "nodiratime"))
			*iopts |= MS_NODIRATIME;
		else if (streq(p, "diratime"))
			*iopts &= ~MS_NODIRATIME;
		else if (streq(p, "auto") || streq(p, "noauto") ||
				 streq(p, "user") || streq(p, "nouser") ||
				 streq(p, "users") || streq(p, "nousers") ||
				 strprefix(p, "fs="))
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


void add_mntoptions(mcond_t *cond, unsigned options)
{
	mntoptions_t *o = xmalloc(sizeof(mntoptions_t)), **oo;

	o->cond = cond;
	o->prio = mcond_prio(cond);
	o->options = options;

	pthread_mutex_lock(&mntoptions_lock);
	/* keep sorted by prio, append at end within same level */
	for(oo = &mntoptions; *oo; oo = &(*oo)->next)
		if ((*oo)->prio > o->prio)
			break;
	o->next = *oo;
	*oo = o;
	pthread_mutex_unlock(&mntoptions_lock);
}

unsigned find_mntoptions(mnt_t *m)
{
	mntoptions_t *o;
	unsigned options = 0;

	pthread_mutex_lock(&mntoptions_lock);
	for(o = mntoptions; o; o = o->next) {
		if (match_mcond(o->cond, m, NULL))
			options |= o->options;
	}
	pthread_mutex_unlock(&mntoptions_lock);
	return options;
}

void purge_mntoptions(void)
{
	mntoptions_t **oo;

	pthread_mutex_lock(&mntoptions_lock);
	oo = &mntoptions;
	while(*oo) {
		mntoptions_t *o = *oo;
		*oo = o->next;
		free_mcond(o->cond);
		free(o);
	}
	pthread_mutex_unlock(&mntoptions_lock);
}

