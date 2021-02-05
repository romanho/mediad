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
#include <libudev.h>
#include "mediad.h"

#ifndef MNT_DETACH
#define MNT_DETACH 2
#endif


struct udev *udev;
pthread_attr_t thread_detached;
sigset_t termsigs;
int volatile shutting_down = 0;

static mnt_t *mounts = NULL;
static pthread_mutex_t mounts_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutexattr_t rec_mutex;

static int by_dirname(mnt_t *m, const void *arg)
{
	return strcmp(m->dir, (const char*)arg) == 0;
}
static int by_dev(mnt_t *m, const void *arg)
{
	return strcmp(m->dev, (const char*)arg) == 0;
}
static int by_devpath(mnt_t *m, const void *arg)
{
	return m->devpath ? strcmp(m->devpath, (const char*)arg) == 0 : 0;
}
static int by_ptr(mnt_t *m, const void *arg)
{
	return m == (const mnt_t*)arg;
}

static mnt_t *get_mount(int (*func)(mnt_t*, const void*),
						const void *arg, int retw_mounts_lock, unsigned tries)
{
	mnt_t *m;
	unsigned try;

	for(try = 0; ; ++try) {
	  
		pthread_mutex_lock(&mounts_lock);

		for(m = mounts; m; m = m->next) {
			if (func(m, arg))
				break;
		}
		if (!m) {
			if (!retw_mounts_lock)
				pthread_mutex_unlock(&mounts_lock);
			return NULL;
		}

		if (pthread_mutex_trylock(&m->lock) == 0) {
			if (!retw_mounts_lock)
				pthread_mutex_unlock(&mounts_lock);
			return m;
		}

		if (tries && try >= tries) {
			if (!retw_mounts_lock)
				pthread_mutex_unlock(&mounts_lock); 
			return NULL;
		}

		pthread_mutex_unlock(&mounts_lock);

		usleep(tries ? 500000 : 50000);
	}
}

int do_mount(const char *name)
{
	mnt_t *m;
	int forced_ro = 0;
	const char *options;
	char path[strlen(autodir)+strlen(name)+2];

	if (!(m = get_mount(by_dirname, name, 0, 0)))
		return -1;

	if (m->mounted) {
		debug("%s already mounted by another thread", name);
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
			// XXX soll nicht no_medium() die Aliasse verwalten?
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

	if (!(m = get_mount(by_dirname, name, 0, 0)))
		return -1;
	if (!m->mounted) {
		debug("%s already unmounted by another thread", name);
		pthread_mutex_unlock(&m->lock);
		return 0;
	}
	if (m->no_automount) {
		pthread_mutex_unlock(&m->lock);
		return -1;
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
		error("symlink(%s,%s): %s", linkto, path, strerror(errno));
	
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

	if (!(par = get_mount(by_devpath, fname+4, 0, 6))) {
		warning("parent device (devpath=%s) for %s not found!", fname+4, m->dev);
		return;
	}

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

	if (!(m = get_mount(by_ptr, mnt, 0, 0)))
		/* mount has disappeared... */
		return NULL;
	
	if (m->n_children) {
		/* children have appeared, message should be suppressed */
		m->suppress_message = 1;
		if (m->serial)
			lpmsg("new %s available (serial number is %s)", m->dir, m->serial);
		goto out;
	}
	/* still exists and no children -> print message */
	msg("new %s/%s available (no filesystem)", autodir, m->dir);
	if (!m->parent && m->serial)
		lpmsg("(serial number is %s)", m->serial);

  out:
	pthread_mutex_unlock(&m->lock);
	return NULL;
}

static void add_mount(const char *dev, const char *perm_alias,
					  unsigned n, char **ids)
{
	mnt_t *m;
	unsigned i, mpres;
	char *msgbuf;
	unsigned options;

	debug("add request for %s", dev);
	/* try to re-read config file if it has changed */
	read_config();
	
	if ((m = get_mount(by_dev, dev, 1, 0))) {
		debug("device %s already existed, replacing it", dev);
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
	}
	pthread_mutex_unlock(&mounts_lock);

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
		get_dev_infos(m);

	/* add permanent alias only if different from mountpoint */
	if (perm_alias && perm_alias[0] && strcmp(perm_alias, m->dir) != 0)
		mnt_add_alias(m, perm_alias, AF_PERM);
	mnt_add_model_alias(m);
	mnt_add_label_alias(m, 0);
	mnt_add_uuid_alias(m, 0);
	match_aliases(m, 0, 0);

	options = find_mntoptions(m);
	if (options & MOPT_NO_AUTOMOUNT)
		m->no_automount = 1;

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
	else {
		msg("new %s/%s available (%s)", autodir, m->dir, msgbuf);
		if (!m->parent && m->serial)
			lpmsg("(serial number is %s)", m->serial);
	}
	mk_dir(m);
	mk_aliases(m, m->type ? WAT_ALL : WAT_NONSPEC);
	pthread_mutex_unlock(&m->lock);

	if (m->no_automount)
		do_mount(dev_to_dir(dev));
}

static void rm_mount(const char *dev)
{
	mnt_t *m, **mm;
	char path[PATH_MAX];
	int try = 6;

	debug("remove request for %s", dev);

  again:
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
	if (pthread_mutex_trylock(&m->lock)) {
		debug("trylock13 %x %s failed", &m->lock, m->dev);
		pthread_mutex_unlock(&mounts_lock);
		usleep(50000);
		goto again;
	}	
	if (m->n_children) {
		debug("%s is Parent and has %d children! sleep 1 second; %d tries",
			  dev, m->n_children, 6-try);
		pthread_mutex_unlock(&m->lock);
		pthread_mutex_unlock(&mounts_lock);
		usleep(500000);
		if (try--)
			goto again;
		else
			return;
	}
	*mm = m->next;
	pthread_mutex_unlock(&mounts_lock);

	mkpath(path, m->dir);
	if (umount(path) == 0)
		dec_mounted();
	else if (errno == EBUSY) {
			umount2(path, MNT_DETACH);
			warning("%s still busy, will be unmounted later", path);
			dec_mounted();
	}
	else if (errno != EINVAL && errno != ENOENT)
		warning("umount(%s): %s", path, strerror(errno));

  again2:
	if (m->parent) {
		mnt_t *p = m->parent;

		if (pthread_mutex_trylock(&p->lock)) {
			debug("trylock15 %x failed", &p->lock);
			usleep(50000);
			goto again2;
		}
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
	xfree(&m->label);
	xfree(&m->type);
	xfree(&m->uuid);
	mnt_free_aliases(m, 0, 0);
	pthread_mutex_destroy(&m->lock);
	free(m);
}

static void *handle_cmd(void *arg)
{
	int fd = (long)arg;
	char cmd;
	char *dev, **ids = NULL;
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
		for(i = 0; i < n; ++i) {
			recv_str(fd, ids+i);
			replace_untrusted_chars(ids[i]);
		}
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

void add_mount_with_devpath(const char *devname, const char *devpath)
{
	char *ids[1];
	ids[0] = alloca(strlen("DEVPATH=")+strlen(devpath)+1);
	sprintf(ids[0], "DEVPATH=%s", devpath);
	add_mount(devname, NULL, 1, ids);
}

static void *scan_fstab(void *dummy)
{
	FILE *f;
	mntent_list_t m;
	int al = strlen(autodir);

	pthread_sigmask(SIG_BLOCK, &termsigs, NULL);

#if 0
	if (!(f = setmntent(ETC_FSTAB, "r")))
		return NULL;
	while(getmntent_r(f, &m.ent, m.buf, sizeof(m.buf))) {
		if (strncmp(m.ent.mnt_dir, autodir, al) == 0 &&
			m.ent.mnt_dir[al] == '/' &&
			hasmntopt(&m.ent, "noauto")) {
			debug("found mount %s -> %s with options %s in /etc/fstab",
				  m.ent.mnt_fsname, m.ent.mnt_dir, m.ent.mnt_opts);
			// XXX: must handle UUID= or LABEL= mnt_fsname from fstab
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
#endif

	coldplug();

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

int used_sigs[] = { SIGHUP, SIGCHLD, SIGINT, SIGQUIT, SIGTERM, 0 };

int daemon_main(void)
{
	int listen_fd;
	
	openlog("mediad", LOG_NDELAY|LOG_PID|LOG_CONS, LOG_DAEMON);
	setpgrp();
	chdir("/");
	if (!(udev = udev_new())) {
		error("failed to create udev context");
		return 1;
	}
	sigemptyset(&termsigs);
	sigaddset(&termsigs, SIGINT);
	sigaddset(&termsigs, SIGQUIT);
	sigaddset(&termsigs, SIGTERM);
	pthread_sigmask(SIG_BLOCK, &termsigs, NULL);

	read_config();
	listen_fd = open_socket();
	start_automount(autodir);
	signal(SIGHUP, SIG_IGN);
	signal(SIGCHLD, SIG_IGN);
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

	udev_unref(udev);
	return 0;
}
