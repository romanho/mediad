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
#include <syslog.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <linux/types.h>
#include <linux/auto_fs4.h>
#include "mediad.h"


static void daemon_ok(int signo)
{
}

static void daemon_failed(int signo)
{
	unlink(SOCKLOCK);
	fatal("daemon failed to start");
}

static void start_daemon(void)
{
	int fd;
	pid_t pid;
	sigset_t blocksigs, oldsigs;

	signal(SIGUSR1, daemon_ok);
	signal(SIGCHLD, daemon_failed);
	sigemptyset(&blocksigs);
	sigaddset(&blocksigs, SIGUSR1);
	sigaddset(&blocksigs, SIGCHLD);
	sigprocmask(SIG_BLOCK, &blocksigs, &oldsigs);

	if ((fd = open(SOCKLOCK, O_CREAT|O_EXCL, 0600)) < 0) {
		if (errno == EEXIST) {
			/* another daemon has already been launched, but it hasn't opened
			 * the listening socket yet... simply wait a bit */
			sigprocmask(SIG_SETMASK, &oldsigs, NULL);
			sleep(2);
			return;
		}
		fatal("%s: %s", SOCKLOCK, strerror(errno));
	}
	close(fd);
	
	if ((pid = fork()) < 0) {
		unlink(SOCKLOCK);
		fatal("fork: %s", strerror(errno));
	}
	else if (pid == 0) {
		int i;
		for(i = 0; i < 256; ++i)
			close(i);
		sigprocmask(SIG_SETMASK, &oldsigs, NULL);
		daemon_main();
		exit(0);
	}
	else {
		/* wait until daemon signals that it's running */
		sigsuspend(&oldsigs);
		sigprocmask(SIG_SETMASK, &oldsigs, NULL);
		unlink(SOCKLOCK);
	}
}


static void test_sock(void)
{
	int sock;
	struct sockaddr_un sa;

	if ((sock = socket(PF_UNIX, SOCK_STREAM, 0)) < 0)
		fatal("socket: %s", strerror(errno));

	sa.sun_family = AF_UNIX;
	strcpy(sa.sun_path, SOCKNAME);
	if (connect(sock, (struct sockaddr*)&sa, sizeof(sa)) < 0) {
		if (errno != ENOENT && errno != ECONNREFUSED)
			fatal("connect: %s", strerror(errno));
		/* no daemon running, start one */
		printf("Starting daemon... ");
		fflush(stdout);
		start_daemon();
		printf("done\n");
	}
	else {
		printf("Daemon already running.\n");
	}
	close(sock);
}

static void send_cmd(char cmd, const char *dev, unsigned n, const char *ids[])
{
	int sock;
	unsigned i;
	struct sockaddr_un sa;
	
	if ((sock = socket(PF_UNIX, SOCK_STREAM, 0)) < 0)
		fatal("socket: %s", strerror(errno));

	sa.sun_family = AF_UNIX;
	strcpy(sa.sun_path, SOCKNAME);
	if (connect(sock, (struct sockaddr*)&sa, sizeof(sa)) < 0) {
		if (errno != ENOENT && errno != ECONNREFUSED)
			fatal("connect: %s", strerror(errno));
		/* no daemon running, start one */
		/* There's actually a race here if multiple instances of main process
		 * are started simultanously and the first started daemon hasn't
		 * created the socket yet. But this is non-critical, as a second
		 * daemon will  */
		start_daemon();
		if (connect(sock, (struct sockaddr*)&sa, sizeof(sa)) < 0)
			fatal("connect: %s", strerror(errno));
	}

	if (write(sock, &cmd, 1) != 1)
		warning("socket write error: %s", strerror(errno));
	send_str(sock, dev);
	send_num(sock, n);
	for(i = 0; i < n; ++i)
		send_str(sock, ids[i]);
	close(sock);
}

int main(int argc, char *argv[], char **env)
{
	const char *action, *devname;

	openlog("mediad-if", LOG_CONS|LOG_ODELAY|LOG_PERROR, LOG_DAEMON);
	if (geteuid() != 0)
		fatal("You must be root");
	if (argc == 2 && streq(argv[1], "start")) {
		/* fix command line shown in ps by overwriting argv[] */
		const char *newname = "mediad";
		unsigned nlen = strlen(newname)+1;
		unsigned clen = argv[argc-1] + strlen(argv[argc-1]) - argv[0];
		if (nlen > clen)
			nlen = clen;
		memcpy(argv[0], newname, nlen);
		memset(argv[0]+nlen, 0, clen-nlen);
		test_sock();
		return 0;
	}
	if (!(action = getenv("ACTION")))
		fatal("Environment variable 'ACTION' not set");
	if (!streq(action, "add") && !streq(action, "remove"))
		fatal("ACTION must be 'add' or 'remove'");
	if (!(devname = getenv("DEVNAME")))
		fatal("Environment variable 'DEVNAME' not set");
	
	if (streq(action, "add")) {
		unsigned i, n = 0;
		const char *ids[MAX_IDS];

		for(i = 0; env[i] && n < MAX_IDS-1; ++i) {
			if (strprefix(env[i], "ID_") ||
				strprefix(env[i], "DEVPATH="))
				ids[n++] = env[i];
		}
		ids[n] = NULL;
		send_cmd('+', devname, n, ids); 
	}
	else {
		send_cmd('-', devname, 0, NULL);
	}
	return 0;
}
