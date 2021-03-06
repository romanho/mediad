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
#include <syslog.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <netinet/in.h>
#include <linux/version.h>
#include <sched.h>
#include <execinfo.h>
#include "mediad.h"


void logit(int pri, const char *fmt, ...)
{
	va_list argp;

	va_start(argp, fmt);
	vsyslog(pri, fmt, argp);
	va_end(argp);
}

void *xmalloc(size_t sz)
{
	void *p = malloc(sz);
	if (!p)
		fatal("out of memory");
	return p;
}

void *xrealloc(void *p, size_t sz)
{
	p = realloc(p, sz);
	if (!p)
		fatal("out of memory");
	return p;
}

char *xstrdup(const char *str)
{
	char *p;

	if (!str)
		return NULL;
	p = strdup(str);
	if (!p)
		fatal("out of memory");
	return p;
}

void xfree(const char **p)
{
	if (*p) {
		free((char*)*p);
		*p = NULL;
	}
}


char *mkpath(char *buf, const char *add)
{
	char *p;
	
	strcpy(buf, autodir);
	strcat(buf, "/");
	p = buf+strlen(buf);
	if (add)
		strcpy(p, add);
	return p;
}

#define CHARS_ALNUM \
	"abcdefghijklmnopqrstuvwxyz" \
	"ABCDEFGHIJKLMNOPQRSTUVWXYZ" \
	"0123456789_"

size_t is_name_eq_val(const char *str)
{
	size_t n = strspn(str, CHARS_ALNUM);
	return (n > 0 && str[n] == '=') ? n : 0;
}

#define Iget(name,field)									\
	do {													\
		const char *__p;									\
		if ((__p = strprefix(line, name)) && *__p == '=') {	\
			xfree(&m->field);								\
			if (__p[1]) {									\
				m->field = xstrdup(__p+1);					\
				debug("found %s = '%s'", #field, m->field);	\
			}												\
			return;											\
		}													\
	} while(0)

void parse_id(mnt_t *m, const char *line)
{
	Iget("DEVPATH",				devpath);
	Iget("ID_VENDOR",			vendor);
	Iget("ID_MODEL",			model);
	Iget("ID_SERIAL",			serial);
	Iget("ID_FS_TYPE",			type);
	Iget("ID_FS_UUID",			uuid);
	Iget("ID_FS_LABEL",			label);
}

void replace_untrusted_chars(char *p)
{
	for(; *p; ++p) {
		if (*p < ' ' || strchr("!\"&'()*;<>[\\]^`{|}~", *p))
			*p = '_';
	}
}

void mk_dir(mnt_t *m)
{
	char path[PATH_MAX];
	
	mkpath(path, m->dir);
	if (mkdir(path, 0755) && errno != EEXIST)
		error("mkdir(%s): %s", path, strerror(errno));
}

void rm_dir(mnt_t *m)
{
	char path[PATH_MAX];
	
	mkpath(path, m->dir);
	if (rmdir(path))
		if (!shutting_down || errno != EACCES)
			warning("rmdir(%s): %s", path, strerror(errno));
}

void send_num(int fd, unsigned num)
{
	union {
		u_int16_t i;
		unsigned char c[2];
	} u;

	if (num > USHRT_MAX)
		fatal("Number too large (%u)", num);
	u.i = htons(num);
	if (write(fd, u.c, 2) != 2)
		warning("socket write error: %s", strerror(errno));
}

void send_str(int fd, const char *str)
{
	int len = strlen(str)+1;

	send_num(fd, len);
	if (write(fd, str, len) != len)
		warning("socket write error: %s", strerror(errno));
}

unsigned recv_num(int fd)
{
	union {
		u_int16_t i;
		unsigned char c[2];
	} u;
	
	if (read(fd, u.c, 2) != 2)
		fatal("read from cmd socket: %s", strerror(errno));
	return ntohs(u.i);
}

void recv_str(int fd, char **p)
{
	int len = recv_num(fd);

	*p = xmalloc(len);
	if (read(fd, (char*)*p, len) != len)
		fatal("read from cmd socket: %s", strerror(errno));
}

unsigned int linux_version_code(void)
{
	struct utsname my_utsname;
	unsigned int p, q, r;
	char *tmp, *save;

	if (uname(&my_utsname))
		return 0;

	p = q = r = 0;

	tmp = strtok_r(my_utsname.release, ".", &save);
	if (!tmp)
		return 0;
	p = (unsigned int ) atoi(tmp);

	tmp = strtok_r(NULL, ".", &save);
	if (!tmp)
		return KERNEL_VERSION(p, 0, 0);
	q = (unsigned int) atoi(tmp);

	tmp = strtok_r(NULL, ".", &save);
	if (!tmp)
		return KERNEL_VERSION(p, q, 0);
	r = (unsigned int) atoi(tmp);

	return KERNEL_VERSION(p, q, r);
}

#define SYS_CGROUP_UNIFIED	"/sys/fs/cgroup/unified"
#define SYS_CGROUP_LEGACY	"/sys/fs/cgroup/pids"

/* put us to out own mediad.service cgroup */
void cgroup_set(const char *grp)
{
	char path[PATH_MAX], *cgpath;
	FILE *f;

	/* use 'unified' if available (v2), 'pids' controller otherwise */
	if (access(SYS_CGROUP_UNIFIED, F_OK) == 0)
		cgpath = SYS_CGROUP_UNIFIED;
	else if (access(SYS_CGROUP_LEGACY, F_OK) == 0)
		cgpath = SYS_CGROUP_LEGACY;
	else
		/* probably no systemd, no action needed for cgroup */
		return;

	snprintf(path, sizeof(path), "%s/%s", cgpath, grp);
	if (mkdir(path, 0755) < 0 && errno != EEXIST) {
		warning("%s: %s", path, strerror(errno));
		return;
	}

	strcat(path, "/cgroup.procs");
	if (!(f = fopen(path, "w"))) {
		warning("%s: %s", path, strerror(errno));
		return;
	}
	fprintf(f, "%d\n", getpid());
	fclose(f);
}

/* set our mount namespace to that of PID */
void set_mnt_ns(pid_t pid)
{
	char path[PATH_MAX];
	int fd;

	snprintf(path, sizeof(path), "/proc/%d/ns/mnt", pid);
	if ((fd = open(path, O_RDONLY)) < 0)
		fatal("%s: %s", path, strerror(errno));
	if (setns(fd, CLONE_NEWNS) < 0)
		fatal("setns: %s", path, strerror(errno));
	close(fd);
}

/* set our comm name */
void set_comm(const char *c)
{
	char path[PATH_MAX];
	FILE *f;

	snprintf(path, sizeof(path), "/proc/%d/comm", getpid());
	if (!(f = fopen(path, "w")))
		fatal("%s: %s", path, strerror(errno));
	fprintf(f, "%s", c);
	fclose(f);
}

void show_backtrace(void)
{
	const unsigned maxaddr = 32;
	void *btaddr[maxaddr];
	unsigned n, i;
	char **syms;

	n = backtrace(btaddr, maxaddr);
	syms = backtrace_symbols(btaddr, n);
	debug("Backtrace:");
	/* start at 1 to skip our direct caller */
	for(i = 1; i < n; ++i)
		debug("  %s", syms[i]);
	free(syms);
}
