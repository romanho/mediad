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
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include "mediad.h"

#ifndef MNT_DETACH
#define MNT_DETACH 2
#endif


pthread_attr_t thread_detached;
sigset_t termsigs;
int volatile shutting_down = 0;

static mnt_t *mounts = NULL;
static pthread_mutex_t mounts_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutexattr_t rec_mutex;

static mnt_t *find_mount(const char *name)
{
	mnt_t *m;
	
	for(m = mounts; m; m = m->next) {
		if (strcmp(name, m->dir) == 0)
			return m;
	}
	return NULL;
}

int do_mount(const char *name)
{
	mnt_t *m;
	int forced_ro = 0;
	const char *options;
	char path[strlen(autodir)+strlen(name)+2];

	pthread_mutex_lock(&mounts_lock);
	if (!(m = find_mount(name))) {
		debug("%s not found", name);
		pthread_mutex_unlock(&mounts_lock);
		return -1;
	}
	pthread_mutex_lock(&m->lock);
	pthread_mutex_unlock(&mounts_lock);

	if (m->mounted) {
		debug("%s already mounted by another thread");
		pthread_mutex_unlock(&m->lock);
		return 0;
	}
	check_medium_change(m);
	if (!m->type) {
		debug("no filesystem found on %s", m->dev);
		pthread_mutex_unlock(&m->lock);
		return -1;
	}

	if (!(options = find_fsoptions(m)))
		options = DEF_FSOPTIONS;
	mkpath(path, name);
	switch(call_mount(m->dev, path, m->type, options)) {
	  case 0:
		break;
	  case 1: {
		  /* read-only had been forced */
		char *newo = alloca(strlen(options)+4);
		strcpy(newo, options);
		strcat(newo, ",ro");
		options = newo;
		forced_ro = 1;
		break;
	  }
	  default:
		if (no_medium_errno()) {
			set_no_medium_present(m);
			if (m->parent) {
				rm_aliases(m, WAT_FSSPEC);
				mnt_free_aliases(m, AF_FSSPEC, AF_FSSPEC);
			}
		}
		else
			error("mount(%s): %s", m->dev, strerror(errno));
		pthread_mutex_unlock(&m->lock);
		return -1;
	}
	
	m->mounted = 1;
	add_mtab(m->dev, path, m->type, options);
	debug("mounted %s on %s (type %s%s)",
		  m->dev, path, m->type, forced_ro ? ", forced read-only" : "");
	pthread_mutex_unlock(&m->lock);
	inc_mounted();
	return 0;
}

int do_umount(const char *name)
{
	mnt_t *m;
	int err;
	char path[strlen(autodir)+strlen(name)+2];

	pthread_mutex_lock(&mounts_lock);
	if (!(m = find_mount(name))) {
		pthread_mutex_unlock(&mounts_lock);
		return -1;
	}
	pthread_mutex_lock(&m->lock);
	pthread_mutex_unlock(&mounts_lock);

	if (!m->mounted) {
		debug("%s already unmounted by another thread");
		pthread_mutex_unlock(&m->lock);
		return 0;
	}
	
	mkpath(path, name);
	err = umount(path);
	debug("umount %s -> %d", path, err ? errno : 0);

	if (err && errno != EINVAL && errno != EBUSY && errno != ENOENT)
		warning("cannot unmount %s: %s", path, strerror(errno));
	else if (err == 0) {
		m->mounted = 0;
		pthread_mutex_unlock(&m->lock);
		rm_mtab(path);
		dec_mounted();
		return 0;
	}
	pthread_mutex_unlock(&m->lock);
	return -1;
}


static void add_child(mnt_t *p, mnt_t *m)
{
	char path[strlen(autodir)+1+strlen(p->dir)+1+7+1];
	char linkto[3+strlen(m->dir)+1];

	if (m->parent && m->parent == p)
		return;
	if (m->parent) {
		error("add_child: parent already exists (m->p=%p, m->p->dev=%s)",
			  m->parent, m->parent->dev);
		return;
	}

	debug("setting parent of %s to %s", m->dev, p->dev);
	snprintf(path, sizeof(path), "%s/%s/part%02d",
			 autodir, p->dir, m->partition);
	strcpy(linkto, "../");
	strcat(linkto, m->dir);
	if (symlink(linkto, path))
		error("symlink(%s,%s): %s", linkto, path);
	
	m->parent = p;
	p->n_children++;
}

static void rm_child(mnt_t *p, mnt_t *m)
{
	char path[strlen(autodir)+1+strlen(p->dir)+1+7+1];

	if (m->parent != p) {
		error("parent inconsistency (m->p=%p, p=%p)", m->parent, p);
		return;
	}
	debug("dropping parent of %s (%s)", m->dev, p->dev);
	m->parent = NULL;
	if (p->n_children > 0)
		p->n_children--;
	
	snprintf(path, sizeof(path), "%s/%s/part%02d",
			 autodir, p->dir, m->partition);
	if (unlink(path))
		error("unlink(%s): %s", path, strerror(errno));
}

static void check_parent(mnt_t *m)
{
	mnt_t *par;
	char *fname, *p;
	struct stat st;
	unsigned pnum = 0;
	
	if (!m->devpath)
		return;
	fname = alloca(4+strlen(m->devpath)+1+5+1);
	strcpy(fname, "/sys");
	strcat(fname, m->devpath);
	strcat(fname, "/start");
	if (stat(fname, &st))
		/* no .../start entry -> isn't a partition */
		return;

	/* cut off "/start" again */
	*(p = fname+strlen(fname)-6) = '\0';
	/* look for partition number */
	while(p > fname && isdigit(p[-1]))
		--p;
	pnum = strtoul(p, NULL, 10);
	/* cut off last component (partition dev name) */
	if (!(p = strrchr(fname, '/')))
		return;
	*p = '\0';

	/* now find that parent name */
	pthread_mutex_lock(&mounts_lock);
	p = fname + 4;
	for(par = mounts; par; par = par->next) {
		if (strcmp(par->devpath, p) == 0)
			break;
	}
	if (!par) {
		warning("parent device (devpath=%s) for %s not found!",
				p, m->dev);
		pthread_mutex_unlock(&mounts_lock);
		return;
	}

	pthread_mutex_lock(&par->lock);
	pthread_mutex_unlock(&mounts_lock);
	m->partition = pnum;
	add_child(par, m);
	pthread_mutex_unlock(&par->lock);
}

static const char *dev_to_dir(const char *dev)
{
	const char *p = dev;
	char *q, *r;

	if (strncmp(dev, "/dev/", 5) == 0)
		p += 5;
	q = r = xmalloc(strlen(p)+2);
	if (config.hide_device_name)
		*q++ = '.';
	for( ;*p; ++p)
		*q++ = *p == '/' ? '_' : *p;
	*q = '\0';
	return r;
}

static void *delayed_message(void *mnt)
{
	mnt_t *m;

	sleep(1);

	/* check that mnt still exists */
	pthread_mutex_lock(&mounts_lock);
	for(m = mounts; m; m = m->next) {
		if (m == (mnt_t*)mnt) {
			pthread_mutex_lock(&m->lock);
			break;
		}
	}
	pthread_mutex_unlock(&mounts_lock);
	if (!m)
		/* mount has disappeared... */
		goto out;
	if (m->n_children) {
		/* children have appeared, message should be suppressed */
		m->suppress_message = 1;
		goto out;
	}
	/* still exists and no children -> print message */
	msg("new %s/%s available (no filesystem)", autodir, m->dir);

  out:
	pthread_mutex_unlock(&m->lock);
	return NULL;
}

static void add_mount(const char *dev, const char *perm_alias,
					  unsigned n, const char **ids)
{
	mnt_t *m;
	unsigned i, mpres;
	char *msgbuf;

	debug("add request for %s", dev);
	/* try to re-read config file if it has changed */
	read_config();
	
	pthread_mutex_lock(&mounts_lock);
	for(m = mounts; m; m = m->next) {
		if (strcmp(dev, m->dev) == 0)
			break;
	}
	if (m) {
		debug("device %s already existed, replacing it", dev);
		pthread_mutex_lock(&m->lock);
		pthread_mutex_unlock(&mounts_lock);
		/* unchanged: dev, devpath, dir */
		rm_aliases(m, WAT_ALL);
		mnt_free_aliases(m, AF_PERM, 0);
		/* clear type to reprobe */
		xfree(&m->type);
	}
	else {
		m = xmalloc(sizeof(mnt_t));
		memset(m, 0, sizeof(mnt_t));
		pthread_mutex_init(&m->lock, &rec_mutex);
		pthread_mutex_lock(&m->lock);
		m->dev = xstrdup(dev);
		m->dir = dev_to_dir(dev);

		m->next = mounts;
		mounts  = m;
		pthread_mutex_unlock(&mounts_lock);
	}

	for(i = 0; i < n; ++i)
		parse_id(m, ids[i]);
	if (!m->devpath)
		find_devpath(m);
	check_parent(m);
	if (!m->parent)
		m->medium_present = check_medium(m->dev);
	mpres = m->parent ? m->parent->medium_present:m->medium_present;
	if (mpres && !m->type)
		/* if no FS_TYPE passed but there is a medium, run vol_id ourselves */
		run_vol_id(m);

	/* add permanent alias only if different from mountpoint */
	if (perm_alias && perm_alias[0] && strcmp(perm_alias, m->dir) != 0)
		mnt_add_alias(m, perm_alias, AF_PERM);
	if (!config.no_model_alias && m->model && m->model[0]) {
		char *mod = xmalloc(strlen(m->model)+3);
		strcpy(mod, m->model);
		strcat(mod, "%P");
		mnt_add_alias(m, mod, 0);
		free(mod);
	}
	if (!config.no_label_alias)
		mnt_add_alias(m, m->label, AF_FSSPEC);
	match_aliases(m, 0);

	msgbuf = alloca((m->vendor ? strlen(m->vendor) : 0) +
					(m->model ? strlen(m->model) : 0) +
					(m->type ? strlen(m->type) : 0) +
					(m->label ? strlen(m->label) : 0) + 48);
	strcpy(msgbuf, m->vendor ? m->vendor : "");
	if (m->model) {
		if (*msgbuf) strcat(msgbuf, " ");
		strcat(msgbuf, m->model);
	}
	if (*msgbuf) strcat(msgbuf, ", ");
	if (!mpres)
		strcat(msgbuf, "no medium");
	else if (!m->type)
		strcat(msgbuf, "no filesystem");
	else if (m->label && m->label[0])
		sprintf(msgbuf+strlen(msgbuf), "%s filesystem '%s'",
				m->type, m->label);
	else
		sprintf(msgbuf+strlen(msgbuf), "%s filesystem without label",
				m->type);

	if (!m->partition && !m->type) {
		/* delay the message if it looks like a partitioned device,
		 * the printout will be suppressed if children appear */
		pthread_t newthread;
		pthread_create(&newthread, &thread_detached,
					   delayed_message, m);
	}
	else
		msg("new %s/%s available (%s)", autodir, m->dir, msgbuf);
	mk_dir(m);
	mk_aliases(m, m->type ? WAT_ALL : WAT_NONSPEC);
	pthread_mutex_unlock(&m->lock);
}

static void rm_mount(const char *dev)
{
	mnt_t *m, **mm;
	char path[PATH_MAX], *mpnt;

	debug("remove request for %s", dev);
	
	pthread_mutex_lock(&mounts_lock);
	for(mm = &mounts; *mm; mm = &(*mm)->next) {
		if (strcmp(dev, (*mm)->dev) == 0)
			break;
	}
	if (!(m = *mm)) {
		debug("to-be-removed device %s unknown", dev);
		pthread_mutex_unlock(&mounts_lock);
		return;
	}
	pthread_mutex_lock(&m->lock);
	*mm = m->next;
	pthread_mutex_unlock(&mounts_lock);

	mpnt = mkpath(path, m->dir);
	if (umount(path) == 0)
		dec_mounted();
	else if (errno == EBUSY) {
			umount2(path, MNT_DETACH);
			warning("%s still busy, will be unmounted later", path);
			dec_mounted();
	}
	else if (errno != EINVAL && errno != ENOENT)
		warning("umount(%s): %s", path, strerror(errno));

	if (m->parent) {
		mnt_t *p = m->parent;
		pthread_mutex_lock(&p->lock);
		rm_child(p, m);
		pthread_mutex_unlock(&p->lock);
	}
	rm_aliases(m, WAT_ALL);
	rm_dir(m);

	if (!m->suppress_message)
		msg("%s/%s removed", autodir, m->dir);
	free((char*)m->dev);
	xfree(&m->devpath);
	free((char*)m->dir);
	mnt_free_aliases(m, 0, 0);
	pthread_mutex_destroy(&m->lock);
	free(m);
}

static void *handle_cmd(void *arg)
{
	int fd = (long)arg;
	char cmd;
	const char *dev, **ids = NULL;
	unsigned n, i;

	pthread_sigmask(SIG_BLOCK, &termsigs, NULL);
	
	n = read(fd, &cmd, 1);
	if (n == 0) {
		/* EOF */
		close(fd);
		return NULL;
	}
	if (n != 1)
		fatal("read from cmd socket: %s", strerror(errno));
	if (cmd != '+' && cmd != '-')
		fatal("bad command '%c'", cmd);
	recv_str(fd, &dev);
	n = recv_num(fd);
	if (n > 0) {
		ids = alloca(n*sizeof(char*));
		for(i = 0; i < n; ++i)
			recv_str(fd, ids+i);
	}
	close(fd);
	
	if (cmd == '+')
		add_mount(dev, NULL, n, ids);
	else
		rm_mount(dev);

	free((char*)dev);
	for(i = 0; i < n; ++i)
		free((char*)(ids[i]));
	return NULL;
}

static int open_socket(void)
{
	int sock;
	struct sockaddr_un sa;

	unlink(SOCKNAME);
	if ((sock = socket(PF_UNIX, SOCK_STREAM, 0)) < 0)
		fatal("socket: %s", strerror(errno));

	sa.sun_family = AF_UNIX;
	strcpy(sa.sun_path, SOCKNAME);
	if (bind(sock, (struct sockaddr*)&sa, sizeof(sa)) < 0)
		fatal("bind: %s", strerror(errno));
	if (chmod(SOCKNAME, 0600))
		fatal("chmod(%s): %s", SOCKNAME, strerror(errno));
	if (listen(sock, 10) < 0)
		fatal("listen: %s", strerror(errno));
	return sock;
}

static void *scan_fstab(void *dummy)
{
	FILE *f;
	mntent_list_t m;
	int al = strlen(autodir);

	pthread_sigmask(SIG_BLOCK, &termsigs, NULL);

	if (!(f = setmntent(ETC_FSTAB, "r")))
		return NULL;
	while(getmntent_r(f, &m.ent, m.buf, sizeof(m.buf))) {
		if (strncmp(m.ent.mnt_dir, autodir, al) == 0 &&
			m.ent.mnt_dir[al] == '/' &&
			hasmntopt(&m.ent, "noauto")) {
			debug("found mount %s -> %s with options %s in /etc/fstab",
				  m.ent.mnt_fsname, m.ent.mnt_dir, m.ent.mnt_opts);
			add_mount(m.ent.mnt_fsname, m.ent.mnt_dir+strlen(autodir)+1, 0, NULL);
			{
				mcond_t *c;
				c = new_mcond(MWH_MTABDEVNAME, MOP_EQ, m.ent.mnt_fsname);
				if (strcmp(m.ent.mnt_type, "subfs") != 0)
					c->next = new_mcond(MWH_FSTYPE, MOP_EQ, m.ent.mnt_type);
				add_fsoptions(c, m.ent.mnt_opts);
			}
		}
	}
	endmntent(f);
	return NULL;
}

static void do_shutdown(int signr)
{
	msg("received signal %d, shutting down", signr);
	shutting_down = 1;
	prepare_stop_automount();
	
	while(mounts)
		rm_mount(mounts->dev);
	stop_automount(autodir);
	unlink(SOCKNAME);
	debug("daemon exiting");
	exit(0);
}

int daemon_main(void)
{
	int listen_fd;
	
	openlog("mediad", LOG_NDELAY|LOG_PID, LOG_DAEMON);
	setpgrp();
	chdir("/");
	sigemptyset(&termsigs);
	sigaddset(&termsigs, SIGINT);
	sigaddset(&termsigs, SIGQUIT);
	sigaddset(&termsigs, SIGTERM);
	pthread_sigmask(SIG_BLOCK, &termsigs, NULL);

	read_config();
	listen_fd = open_socket();
	start_automount(autodir);
	signal(SIGHUP, SIG_IGN);
	signal(SIGCHLD, SIG_DFL);
	signal(SIGINT, do_shutdown);
	signal(SIGQUIT, do_shutdown);
	signal(SIGTERM, do_shutdown);

	pthread_attr_init(&thread_detached);
	pthread_attr_setdetachstate(&thread_detached, PTHREAD_CREATE_DETACHED);
	pthread_mutexattr_init(&rec_mutex);
	pthread_mutexattr_settype(&rec_mutex, PTHREAD_MUTEX_RECURSIVE_NP);
	
	debug("daemon running");
	kill(getppid(), SIGUSR1);
	pthread_sigmask(SIG_UNBLOCK, &termsigs, NULL);

	if (!config.no_scan_fstab) {
		pthread_t newthread;
		pthread_create(&newthread, &thread_detached,
					   scan_fstab, NULL);
	}
	
	while(!shutting_down) {
		int fd;
		pthread_t newthread;
		
		if ((fd = accept(listen_fd, NULL, NULL)) < 0) {
			warning("accept: %s", strerror(errno));
			continue;
		}
		pthread_create(&newthread, &thread_detached,
					   handle_cmd, (caddr_t)(long)fd);
	}
	return 0;
}
