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
#include <pthread.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <limits.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <linux/types.h>
#include <linux/kd.h>
#include <linux/auto_fs4.h>
#include <linux/version.h>
#include "mediad.h"


const char *autodir = "/media";

static int n_mounted = 0;
static pthread_mutex_t expire_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t expire_cond = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t blink_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t blinker_thread = 0;
static int pfd, ifd;


static int toggle_led(int fd, int led)
{
	char status;
	
	if (ioctl(fd, KDGETLED, &status)) {
		error("KDGETLED: %s", strerror(errno));
		return -1;
	}
	status ^= led;
	if (ioctl(fd, KDSETLED, status)) {
		error("KDSETLED: %s", strerror(errno));
		return -1;
	}
	return 0;
}

#define BLINKDELAY_ON			10
#define BLINKDELAY_OFF_LONG		1440
#define BLINKDELAY_OFF_SHORT	140
#define N_BLINK_END				6

static void *blinker(void *dummy)
{
	int ttyfd;
	int i;

	if ((ttyfd = open("/dev/tty0", O_RDONLY)) < 0) {
		error("/dev/tty: %s", strerror(errno));
		return NULL;
	}
	debug("blinker thread started");

  repeat:
	while(n_mounted > 0) {
		if (toggle_led(ttyfd, config.blink_led))
			break;
		usleep(BLINKDELAY_ON*1000);
		if (toggle_led(ttyfd, config.blink_led))
			break;
		usleep(BLINKDELAY_OFF_LONG*1000);
	}
	debug("blinker thread signalling all unmounted");

	for(i = 0; i < N_BLINK_END; ++i) {
		if (toggle_led(ttyfd, config.blink_led))
			break;
		usleep(BLINKDELAY_ON*1000);
		if (toggle_led(ttyfd, config.blink_led))
			break;
		usleep(BLINKDELAY_OFF_SHORT*1000);
	}

	pthread_mutex_lock(&blink_lock);
	if (n_mounted > 0) {
		pthread_mutex_unlock(&blink_lock);
		debug("blinker thread resuming");
		goto repeat;
	}
	debug("blinker thread exiting");
	close(ttyfd);
	blinker_thread = 0;
	pthread_mutex_unlock(&blink_lock);
	return NULL;
}


void inc_mounted(void)
{
	pthread_mutex_lock(&expire_lock);
	n_mounted++;
	pthread_cond_signal(&expire_cond);

	if (config.blink_led && n_mounted == 1) {
		pthread_mutex_lock(&blink_lock);
		if (!blinker_thread) {
			if (pthread_create(&blinker_thread, &thread_detached,
							   blinker, NULL))
				warning("failed to create blinker thread");
		}
		pthread_mutex_unlock(&blink_lock);
	}
	pthread_mutex_unlock(&expire_lock);
	
	debug("n_mounted=%d", n_mounted);
}

void dec_mounted(void)
{
	pthread_mutex_lock(&expire_lock);
	--n_mounted;
	pthread_mutex_unlock(&expire_lock);
	debug("n_mounted=%d", n_mounted);
}

static void *expire_automounts(void *dummy)
{
	pthread_sigmask(SIG_BLOCK, &termsigs, NULL);
	
	while(!shutting_down) {
		pthread_mutex_lock(&expire_lock);
		while(n_mounted == 0) {
			pthread_cond_wait(&expire_cond, &expire_lock);
		}
		pthread_mutex_unlock(&expire_lock);

		while(n_mounted > 0) {
			int now = AUTOFS_EXP_LEAVES;
			while(ioctl(ifd, AUTOFS_IOC_EXPIRE_MULTI, &now) == 0)
				;
			usleep(config.expire_freq*1000000);
		}
	}
	return NULL;
}

static int send_ack(unsigned int wait_queue_token, int failed)
{
	if (!wait_queue_token)
		return 0;
	if (ioctl(ifd, failed ? AUTOFS_IOC_FAIL : AUTOFS_IOC_READY,
			  wait_queue_token) < 0) {
		warning("AUTOFS_IOC_xxx: %s", strerror(errno));
		return -1;
	}
	return 0;
}

static void *handle_missing(void *_pkt)
{
	struct autofs_packet_missing *pkt = (struct autofs_packet_missing*)_pkt;
	char name[pkt->len+1];

	pthread_sigmask(SIG_BLOCK, &termsigs, NULL);
	
	strncpy(name, pkt->name, pkt->len);
	name[pkt->len] = '\0';
	debug("request for %s", name);
	send_ack(pkt->wait_queue_token, do_mount(name));
	free(pkt);
	return NULL;
}

static void *handle_expire(void *_pkt)
{
	struct autofs_packet_expire_multi *pkt = (struct autofs_packet_expire_multi*)_pkt;
	char name[pkt->len+1];

	pthread_sigmask(SIG_BLOCK, &termsigs, NULL);
	
	strncpy(name, pkt->name, pkt->len);
	name[pkt->len] = '\0';
	send_ack(pkt->wait_queue_token, do_umount(name));
	free(pkt);
	return NULL;
}

static int read_kernel_packet(int fd, union autofs_packet_union *pkt)
{
	char *p;
	size_t len = sizeof(struct autofs_packet_hdr);
	size_t n;
	static int kern_vers = -1;

	if (kern_vers < 0)
		kern_vers = linux_version_code();

	if (kern_vers >= KERNEL_VERSION(3, 3, 0)) {
		len = sizeof(struct autofs_v5_packet);
	  repeat:
		n = read(fd, pkt, len);
		if (n < 0) {
			if (errno == EINTR)
				goto repeat;
			else
				fatal("pipe read error: %s", strerror(errno));
		}	
		if (n == 0)
			return -1;
		if (n < sizeof(struct autofs_packet_hdr))
			fatal("pipe short read (<hdr)");
		return 0;
	}
	
	/* read header first (actual length determined by packet type!) */
	for(p = (char *)pkt; len > 0; ) {
		n = read(fd, p, len);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			else
				fatal("pipe read error: %s", strerror(errno));
		}	
		if (n == 0)
			return -1;
		p += n;
		len -= n;
	}
	
	switch(pkt->hdr.type) {
	  case autofs_ptype_missing:
		len = sizeof(struct autofs_packet_missing); break;
	  case autofs_ptype_expire:
		len = sizeof(struct autofs_packet_expire); break;
	  case autofs_ptype_expire_multi:
		len = sizeof(struct autofs_packet_expire_multi); break;
	  default:
		fatal("autofs: unknown packet type %d received from kernel");
	}
	len -= sizeof(struct autofs_packet_hdr);

	while(len > 0) {
		size_t n = read(fd, p, len);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			else
				fatal("pipe read error: %s", strerror(errno));
		}	
		if (n == 0)
			return -1;
		p += n;
		len -= n;
	}
	return 0;
}

static void *handle_autofs_events(void *dummy)
{
	union autofs_packet_union pkt, *pktp;

	pthread_sigmask(SIG_BLOCK, &termsigs, NULL);
	
	while(!shutting_down) {
		pthread_t newthread;

		if (read_kernel_packet(pfd, &pkt) < 0)
			return NULL;

		switch(pkt.hdr.type) {
		  case autofs_ptype_missing:
			pktp = xmalloc(sizeof(union autofs_packet_union));
			*pktp = pkt;
			if (pthread_create(&newthread, &thread_detached,
							   handle_missing, pktp))
				warning("failed to create mount thread");
			break;
		  case autofs_ptype_expire_multi:
			pktp = xmalloc(sizeof(union autofs_packet_union));
			*pktp = pkt;
			if (pthread_create(&newthread, &thread_detached,
							   handle_expire, pktp))
				warning("failed to create umount thread");
			break;
		  default:
			warning("unknown autofs packet type %d from kernel",
					pkt.hdr.type);
		}
	}
	return NULL;
}

void start_automount(const char *dir)
{
	int pipefd[2];
	char options[64];
	char mountname[64];
	int kproto_major;
	pthread_t expire_thread;
	pthread_t handler_thread;
	
	debug("mouting autofs for %s", dir);
	/* create a pipe for communication with kernel */
	if (pipe(pipefd) < 0)
		fatal("pipe: %s", strerror(errno));
	/* new mount a new autofs on our directory */
	sprintf(options, "fd=%d,pgrp=%d,minproto=4,maxproto=4",
			pipefd[1], getpgrp());
	sprintf(mountname, "mediad(pid%d)", getpid());
	add_mtab(mountname, dir, "autofs", options);
	if (mount(mountname, dir, "autofs", 0, options) < 0)
		fatal("mount(%s,%s): %s", mountname, dir, strerror(errno));
	close(pipefd[1]);
	pfd = pipefd[0];
	
	/* fd on root dir for ioctls */
	if ((ifd = open(dir, O_RDONLY)) < 0) {
		umount(dir);
		fatal("%s: %s", dir, strerror(errno));
	}
	if (ioctl(ifd, AUTOFS_IOC_PROTOVER, &kproto_major)) {
		umount(dir);
		fatal("AUTOFS_IOC_PROTOVER: %s", strerror(errno));
	}
	if (kproto_major < 4)
		fatal("kernel autofs protocol too old (< 4.x)");
#if 0
	if (kproto_major == 4) {
		if (ioctl(ifd, AUTOFS_IOC_PROTOSUBVER, &kproto_minor) ||
			kproto_minor < 1) {
			umount(dir);
			fatal("kernel autofs protocol too old (4.%d < 4.1)", kproto_minor);
		}
	}
#endif
	if (kproto_major > AUTOFS_MAX_PROTO_VERSION)
		fatal("kernel autofs protocol too new (%d > %d)",
			  kproto_major, AUTOFS_MAX_PROTO_VERSION);
    if (ioctl(ifd, AUTOFS_IOC_SETTIMEOUT, &config.expire_timeout)) {
		umount(dir);
		fatal("AUTOFS_IOC_SETTIMEOUT: %s", strerror(errno));
	}		

	if (pthread_create(&expire_thread, &thread_detached,
					   expire_automounts, NULL))
		fatal("failed to create expire thread");
	if (pthread_create(&handler_thread, &thread_detached,
					   handle_autofs_events, NULL))
		fatal("failed to create autofs handler thread");
}

void prepare_stop_automount(void)
{
	ioctl(ifd, AUTOFS_IOC_CATATONIC, 0);
}

void stop_automount(const char *dir)
{
	close(ifd);
	close(pfd);
	if (umount(dir))
		error("umount(%s): %s", dir, strerror(errno));
	rm_mtab(dir);
}
