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

void match_aliases(mnt_t *m, int fsspec_only, unsigned flags)
{
	alias_t *a;
	int fsspec;

	pthread_mutex_lock(&aliases_lock);
	for(a = aliases; a; a = a->next) {
		if (match_mcond(a->cond, m, &fsspec))
		  if (!fsspec_only || fsspec)
			  mnt_add_alias(m, a->alias, flags | (fsspec ? AF_FSSPEC : 0));
	}
	pthread_mutex_unlock(&aliases_lock);
}

void mark_aliases(mnt_t *m, unsigned mask, unsigned flags, unsigned newflags)
{
	alist_t *a;

	for(a = m->aliases; a; a = a->next) {
		if ((a->flags & mask) == flags) {
			a->flags |= newflags;
		}
	}
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

static int expand_uniq(char *dst, const char *src, unsigned n)
{
	const char *p = strstr(src, "%u");
	unsigned l;

	if (!p) {
		strcpy(dst, src);
		return 0;
	}

	l = p-src;
	strncpy(dst, src, l);
	dst[l] = '\0';
	if (n)
		sprintf(dst+l, "#%u", n);
	if (p[2])
		strcat(dst+l, p+2);
	return 1;
}

void mk_aliases(mnt_t *m, whatalias_t fsspec)
{
	alist_t *a;
	char path[PATH_MAX], *mpnt;
	
	mpnt = mkpath(path, m->dir);
	for(a = m->aliases; a; a = a->next) {
		unsigned u_cnt = 0;
		int have_uniq;
		if ((a->flags & AF_FSSPEC) ?
			(fsspec == WAT_NONSPEC) : (fsspec == WAT_FSSPEC))
			continue;
		if (!a->name[0] || a->created)
			continue;

	  repeat:
		have_uniq = expand_uniq(mpnt, a->name, u_cnt);
		if (symlink(m->dir, path)) {
			if (errno == EEXIST && have_uniq) {
				++u_cnt;
				goto repeat;
			}
			warning("symlink(%s,%s): %s", m->dir, path, strerror(errno));
		}
		else {
			a->created = xstrdup(path);
			debug("linked alias '%s' to %s", mpnt, m->dev);
		}
	}
}

static void rm_alias(mnt_t *m, alist_t *a)
{
	if (!a->created)
		return;

	if (unlink(a->created)) {
		if (!shutting_down || errno != EACCES)
			warning("unlink(%s): %s", a->created, strerror(errno));
		return;
	}
	debug("removed alias '%s' to %s", a->created, m->dev);
	free((char*)(a->created));
	a->created = NULL;
}

void rm_aliases(mnt_t *m, whatalias_t fsspec)
{
	alist_t *a;

	for(a = m->aliases; a; a = a->next) {
		if ((a->flags & AF_FSSPEC) ?
			(fsspec == WAT_NONSPEC) : (fsspec == WAT_FSSPEC))
			continue;
		rm_alias(m, a);
	}
}

void mnt_add_alias(mnt_t *m, const char *a, unsigned flags)
{
	alist_t *al;
        
	if (!a || !*a)
		return;

	/* if the OLD flag is given, search for an existing alias with the same
	 * name; if found, clear its OLD flag so that it won't be removed */
	if (flags & AF_OLD) {
		for(al = m->aliases; al; al = al->next) {
			if (streq(al->name, a)) {
				al->flags &= ~AF_OLD;
				return;
			}
		}
	}
	
	al = xmalloc(sizeof(alist_t));
	al->name = expand_alias(a, m->partition);
	al->flags = flags;
	al->created = NULL;
	al->next = m->aliases;
	m->aliases = al;
	debug("added alias %s to %s", a, m->dev);
}

void mnt_free_aliases(mnt_t *m, unsigned mask, unsigned flags)
{
	alist_t **a = &m->aliases;
	char path[PATH_MAX];

	mkpath(path, m->dir);

	while(*a) {
		if (((*a)->flags & mask) == flags) {
			alist_t *p = *a;
			*a = p->next;

			rm_alias(m, p);
			free((char*)(p->name));
			free(p);
		}
		else
			a = &(*a)->next;
	}
}

void mnt_add_model_alias(mnt_t *m)
{
	char *p;
	
	if (config.no_model_alias || !m->model || !m->model[0])
		return;
	
	p = alloca(strlen(m->model)+5);
	strcpy(p, m->model);
	strcat(p, "%u%P");
	mnt_add_alias(m, p, 0);
}

void mnt_add_label_alias(mnt_t *m, unsigned flags)
{
	if (config.no_label_alias || !m->label || !m->label[0])
		return;
	
	if (!config.no_label_unique) {
		char *p = alloca(strlen(m->label)+3);
		strcpy(p, m->label);
		strcat(p, "%u");
		mnt_add_alias(m, p, flags | AF_FSSPEC);
	}
	else
		mnt_add_alias(m, m->label, flags | AF_FSSPEC);
}

void mnt_add_uuid_alias(mnt_t *m, unsigned flags)
{
	char *p;

	if (!config.uuid_alias || !m->uuid || !m->type || !m->uuid[0] || !m->type[0])
		return;
	
	p = alloca(strlen(m->type)+1+strlen(m->uuid)+1);
	strcpy(p, m->type);
	strcat(p, ":");
	strcat(p, m->uuid);
	mnt_add_alias(m, p, flags | AF_FSSPEC);
}
