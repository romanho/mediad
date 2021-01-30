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


static alias_t *aliases = NULL;
static pthread_mutex_t aliases_lock = PTHREAD_MUTEX_INITIALIZER;


void add_alias(mcond_t *cond, const char *alias)
{
	alias_t *a = xmalloc(sizeof(alias_t));
	
	a->cond  = cond;
	a->alias = xstrdup(alias);

	pthread_mutex_lock(&aliases_lock);
	a->next  = aliases;
	aliases  = a;
	pthread_mutex_unlock(&aliases_lock);
}

void purge_aliases(void)
{
	alias_t *a;

	pthread_mutex_lock(&aliases_lock);
	while((a = aliases)) {
		aliases = a->next;
		free_mcond(a->cond);
		free((char*)a->alias);
		free(a);
	}
	pthread_mutex_unlock(&aliases_lock);
}

void match_aliases(mnt_t *m, int fsspec_only)
{
	alias_t *a;
	int fsspec;

	pthread_mutex_lock(&aliases_lock);
	for(a = aliases; a; a = a->next) {
		if (match_mcond(a->cond, m, &fsspec))
			if (!fsspec_only || fsspec)
				mnt_add_alias(m, a->alias, fsspec ? AF_FSSPEC : 0);
	}
	pthread_mutex_unlock(&aliases_lock);
}

static const char *expand_alias(const char *a, unsigned partno)
{
	const char *p;
	
	if ((p = strstr(a, "%p"))) {
		char *q = xmalloc(strlen(a)+16-2+1);
		strncpy(q, a, p-a);
		q[p-a] = '\0';
		if (partno)
			sprintf(q+(p-a), "%u", partno);
		strcat(q, p+2);
		return q;
	}
	else if ((p = strstr(a, "%P"))) {
		char *q = xmalloc(strlen(a)+21-2+1);
		strncpy(q, a, p-a);
		q[p-a] = '\0';
		if (partno)
			sprintf(q+(p-a), "-part%u", partno);
		strcat(q, p+2);
		return q;
	}
	
	return xstrdup(a);
}

void mnt_add_alias(mnt_t *m, const char *a, unsigned flags)
{
	alist_t *al;
	
	if (!a || !*a)
		return;
	al = xmalloc(sizeof(alist_t));
	al->name = expand_alias(a, m->partition);
	al->flags = flags;
	al->next = m->aliases;
	m->aliases = al;
	debug("added alias %s to %s", a, m->dev);
}

void mnt_free_aliases(mnt_t *m, unsigned mask, unsigned flags)
{
	alist_t **a = &m->aliases;

	while(*a) {
		if (((*a)->flags & mask) == flags) {
			alist_t *p = *a;
			*a = p->next;
			free((char*)(p->name));
			free(p);
		}
		else
			a = &(*a)->next;
	}
}

void mk_aliases(mnt_t *m, whatalias_t fsspec)
{
	alist_t *a;
	char path[PATH_MAX], *mpnt;
	
	mpnt = mkpath(path, m->dir);
	for(a = m->aliases; a; a = a->next) {
		if ((a->flags & AF_FSSPEC) ?
			(fsspec == WAT_NONSPEC) : (fsspec == WAT_FSSPEC))
			continue;
		if (!a->name[0])
			continue;
		strcpy(mpnt, a->name);
		if (symlink(m->dir, path))
			warning("symlink(%s,%s): %s", m->dir, path, strerror(errno));
		debug("linked alias '%s' to %s", a->name, m->dev);
	}
}

void rm_aliases(mnt_t *m, whatalias_t fsspec)
{
	alist_t *a;
	char path[PATH_MAX], *mpnt;
	
	mpnt = mkpath(path, m->dir);
	for(a = m->aliases; a; a = a->next) {
		if ((a->flags & AF_FSSPEC) ?
			(fsspec == WAT_NONSPEC) : (fsspec == WAT_FSSPEC))
			continue;
		if (!a->name[0])
			continue;
		strcpy(mpnt, a->name);
		if (unlink(path))
			warning("unlink(%s): %s", path, strerror(errno));
		debug("removed alias '%s' to %s", a->name, m->dev);
	}
}

