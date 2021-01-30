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
#include <syslog.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <mntent.h>
#include "mediad.h"


typedef struct _mtab_wq {
	struct _mtab_wq *next;
	int             add;
	const char      *dev;
	const char      *dir;
	const char      *fstype;
	const char      *options;
} mtab_wq_t;

static mtab_wq_t *wqueue = NULL;
static pthread_t replay_thread;
static pthread_mutex_t mtab_lock = PTHREAD_MUTEX_INITIALIZER;


static void _add_mtab(const char *dev, const char *dir,
					  const char *fstype, const char *options)
{
	FILE *f;
	struct mntent ent;
	
	if (!(f = setmntent(ETC_MTAB, "a"))) {
		warning("can't open %s: %s", ETC_MTAB, strerror(errno));
		return;
	}
	ent.mnt_fsname = (char*)dev;
	ent.mnt_dir    = (char*)dir;
	ent.mnt_type   = (char*)fstype;
	ent.mnt_opts   = (char*)options;
	ent.mnt_freq   = 0;
	ent.mnt_passno = 0;
	addmntent(f, &ent);
	fclose(f);
}

static void _rm_mtab(const char *dir)
{
	FILE *f;
	mntent_list_t *m, *mlist = NULL, **mend = &mlist;

	if (!(f = setmntent(ETC_MTAB, "r"))) {
		warning("can't open %s: %s", ETC_MTAB, strerror(errno));
		return;
	}
	
	m = xmalloc(sizeof(mntent_list_t));
	while(getmntent_r(f, &m->ent, m->buf, sizeof(m->buf))) {
		m->next = NULL;
		*mend = m;
		mend = &(m->next);
		if (!mlist)
			mlist = m;
		m = xmalloc(sizeof(mntent_list_t));
	}
	free(m);
	endmntent(f);

	if (!(f = setmntent(ETC_MTAB, "w"))) {
		warning("can't open %s: %s", ETC_MTAB, strerror(errno));
		goto out;
	}
	for(m = mlist; m; m = m->next) {
		if (strcmp(m->ent.mnt_dir, dir) != 0)
			addmntent(f, &m->ent);
	}
	fclose(f);

  out:
	while((m = mlist)) {
		mlist = m->next;
		free(m);
	}
}


static int lock_mtab(void)
{
	static int mtab_ok = -1;
	struct stat st;
	int try, fd;
	char target[64];
	struct flock flock;

	pthread_mutex_lock(&mtab_lock);
	if (mtab_ok < 0) {
		if (lstat(ETC_MTAB, &st))
			mtab_ok = 0;
		else
			mtab_ok = S_ISREG(st.st_mode);
	}
	if (!mtab_ok)
		return ENOENT;

	sprintf(target, ETC_MTAB "~%d", getpid());
	for(try = 0; try < 5; ++try) {
		if ((fd = open(target, O_WRONLY|O_CREAT, 0600)) < 0) {
			if (errno == EROFS)
				return EROFS;
			fatal("%s: %s", target, strerror(errno));
		}
		close(fd);
		if (link(target, ETC_MTAB "~")) {
			if (errno == EEXIST)
				goto retry;
			fatal("link(%s): %s", ETC_MTAB "~", strerror(errno));
		}
		unlink(target);

		if ((fd = open(ETC_MTAB "~", O_WRONLY)) < 0) {
			if (errno == ENOENT)
				goto retry;
			fatal("%s: %s", ETC_MTAB "~", strerror(errno));
		}
		flock.l_type   = F_WRLCK;
		flock.l_whence = SEEK_SET;
		flock.l_start  = 0;
		flock.l_len    = 0;
		if (fcntl(fd, F_SETLK, &flock) == 0) {
			close(fd);
			return 0;
		}

	  retry:
		if (try >= 3)
			sleep(1);
	}
	return EAGAIN;
}

static void unlock_mtab(void)
{
	unlink(ETC_MTAB "~");
	pthread_mutex_unlock(&mtab_lock);
}


static void *wq_replay(void *dummy)
{
	int err;

	/* try to lock mtab, loop until it's writeable or permanently fails */
	for(;;) {
		usleep(500000);
		if ((err = lock_mtab()) != EROFS)
			break;
		pthread_mutex_unlock(&mtab_lock);
	}
	if (!err) {
		while(wqueue) {
			mtab_wq_t *w = wqueue;
			wqueue = w->next;

			if (w->add)
				_add_mtab(w->dev, w->dir, w->fstype, w->options);
			else
				_rm_mtab(w->dir);

			xfree(&w->dev);
			xfree(&w->dir);
			xfree(&w->fstype);
			xfree(&w->options);
			free(w);
		}
		unlock_mtab();
	}
	return NULL;
}

static void wq_add(int add, const char *dev, const char *dir,
				   const char *fstype, const char *options)
{
	mtab_wq_t **p, *old;
	mtab_wq_t *w = xmalloc(sizeof(mtab_wq_t));

	w->next = NULL;
	w->add = add;
	w->dev = xstrdup(dev);
	w->dir = xstrdup(dir);
	w->fstype = xstrdup(fstype);
	w->options = xstrdup(options);

	old = wqueue;
	for(p = &wqueue; *p; p = &(*p)->next)
		;
	*p = w;
	if (!old)
		pthread_create(&replay_thread, &thread_detached,
					   wq_replay, NULL);
}


void add_mtab(const char *dev, const char *dir,
			  const char *fstype, const char *options)
{
	int err;
	
	if ((err = lock_mtab())) {
		if (err == EROFS)
			wq_add(1, dev, dir, fstype, options);
		pthread_mutex_unlock(&mtab_lock);
		return;
	}
	_add_mtab(dev, dir, fstype, options);
	unlock_mtab();
}

void rm_mtab(const char *dir)
{
	int err;

	if ((err = lock_mtab())) {
		if (err == EROFS)
			wq_add(0, NULL, dir, NULL, NULL);
		pthread_mutex_unlock(&mtab_lock);
		return;
	}
	_rm_mtab(dir);
	unlock_mtab();
}
