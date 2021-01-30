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
#include <ctype.h>
#include <errno.h>
#include <sys/stat.h>
#include <linux/kd.h>
#include "mediad.h"


config_t config;
#define FORCE_DEBUG 0

static time_t config_mtime = 0;
static const char *parse_error, *parse_error_arg;
static char *strpool = NULL, *strpoolp;
static int strpool_size;

#define PERRS(str)      do { parse_error = str; parse_error_arg = ""; return NULL; } while(0)
#define PERRSA(str,arg) do { parse_error = str; parse_error_arg = arg; return NULL; } while(0)
#define PERRI(str)      do { parse_error = str; parse_error_arg = ""; return -1; } while(0)
#define PERRIA(str,arg) do { parse_error = str; parse_error_arg = arg; return -1; } while(0)


static void strpool_init(int len)
{
	int sz = 2*len;
	
	if (!strpool || strpool_size < sz)
		strpool = xrealloc(strpool, strpool_size = sz);
	strpoolp = strpool;
}

static void strpool_free(void)
{
	xfree((const char **)&strpool);
}

static char *strpool_push(char *from, char *to)
{
	char *p = strpoolp;
	int len = to-from;

	strncpy(strpoolp, from, len);
	strpoolp[len] = '\0';
	strpoolp += len+1;
	return p;
}


static void skipwhite(char **p)
{
	*p += strspn(*p, " \t");
}

static char *getword(char **p)
{
	char *w;

	skipwhite(p);
	if (!**p)
		PERRS("keyword missing");
	w = *p;
	while(**p && (isalnum(**p) || strchr("-_%", **p)))
		++*p;
	if (*p == w)
		PERRS("not a keyword");
	return strpool_push(w, *p);
}

static char *getstr(char **p)
{
	char *w;
	
	skipwhite(p);
	if (!**p)
		PERRS("expected string missing");
	if (**p != '"')
		return getword(p);
	w = ++*p;
	for(;;) {
		if (!**p)
			PERRSA("no closing quote for string '%s'", w);
		if (**p == '"') {
			w = strpool_push(w, *p);
			++*p;
			break;
		}
		if (**p == '\\' && *(*p+1) == '"')
			memmove(*p, *p+1, strlen(*p));
		++*p;
	}
	return w;
}

static int getassign(char **p)
{
	skipwhite(p);
	if (**p != '=')
		PERRI("missing '='");
	++*p;
	return 0;
}

static int getif(char **p)
{
	char *w;

	skipwhite(p);
	if (!(w = getword(p)) || (strcmp(w, "if") != 0 && strcmp(w, "for") != 0))
		PERRI("missing 'if' or 'for'");
	return 0;
}

static int getbool(char **p)
{
	char *w;

	if (!(w = getword(p)))
		PERRI("boolean value missing");
	if (strcasecmp(w, "y") == 0 || strcasecmp(w, "yes") == 0 ||
		strcasecmp(w, "t") == 0 || strcasecmp(w, "true") == 0 ||
		strcasecmp(w, "on") == 0 || strcmp(w, "1") == 0)
		return 1;
	if (strcasecmp(w, "n") == 0 || strcasecmp(w, "no") == 0 ||
		strcasecmp(w, "f") == 0 || strcasecmp(w, "false") == 0 ||
		strcasecmp(w, "off") == 0 || strcmp(w, "0") == 0)
		return 0;
	PERRIA("bad boolean value '%s'", w);
}

static int getnum(char **p)
{
	char *w, *we;
	int val;

	if (!(w = getword(p)))
		PERRI("expected number missing");
	if ((val = strtoul(w, &we, 10)) == 0 || we == w || *we)
		PERRIA("bad number '%s'", w);
	return val;
}

static int getled(char **p)
{
	char *w;
	int val;

	if (!(w = getword(p)))
		PERRI("expected led name missing");
	if (strcasecmp(w, "num") == 0 ||
		strcasecmp(w, "numlock") == 0)
		val = LED_NUM;
	else if (strcasecmp(w, "cap") == 0 ||
			 strcasecmp(w, "caps") == 0 ||
			 strcasecmp(w, "capslock") == 0)
		val = LED_CAP;
	else if (strcasecmp(w, "scr") == 0 ||
			 strcasecmp(w, "scroll") == 0 ||
			 strcasecmp(w, "scrlock") == 0 ||
			 strcasecmp(w, "scrolllock") == 0)
		val = LED_SCR;
	else
		PERRIA("bad led name '%s'", w);
	return val;
}

static mcond_t *getmcond(char **p)
{
	char *w, *value;
	matchwhat_t what;
	matchop_t op;

	if (!(w = getword(p)))
		PERRS("missing keyword for condition");
	if (strcmp(w, "device") == 0)
		what = MWH_DEVNAME;
	else if (strcmp(w, "vendor") == 0)
		what = MWH_VENDOR;
	else if (strcmp(w, "model") == 0)
		what = MWH_MODEL;
	else if (strcmp(w, "serial") == 0)
		what = MWH_SERIAL;
	else if (strcmp(w, "partition") == 0)
		what = MWH_PARTITION;
	else if (strcmp(w, "fstype") == 0)
		what = MWH_FSTYPE;
	else if (strcmp(w, "uuid") == 0)
		what = MWH_UUID;
	else if (strcmp(w, "label") == 0)
		what = MWH_LABEL;
	else
		PERRSA("unknown match condition '%s'", w);

	skipwhite(p);
	if (!((**p == '=' || **p == '!') && *(*p+1) == '='))
		PERRS("bad match operator (expected '==' or '!=')");
	op = (**p == '=') ? MOP_EQ : MOP_NE;
	*p += 2;
	
	if (!(value = getstr(p)))
		return NULL;
	return new_mcond(what, op, value);
}

static mcond_t *getmcondlist(char **p)
{
	mcond_t *c, *conds = NULL;
	
	for(;;) {
		if (!(c = getmcond(p))) {
			free_mcond(conds);
			return NULL;
		}
		c->next = conds;
		conds = c;

		skipwhite(p);
		if (!**p)
			break;
		if (**p == ',') {
			++*p;
			continue;
		}
		if (**p == '&' && *(*p+1) == '&') {
			*p += 2;
			continue;
		}

		free_mcond(conds);
		PERRS("expected ',' or '&&' or end of line after condition");
	}
	return conds;
}

static void parse_line(int lno, char *line)
{
	char *p = line, *w, *w2;
	mcond_t *c;
	int n;

	if (!(w = getword(&p)))
		goto parse_err;
	if (strcmp(w, "scan-fstab") == 0) {
		if (getassign(&p) || (n = getbool(&p)) < 0)
			goto parse_err;
		config.no_scan_fstab = !n;
	}
	else if (strcmp(w, "model-alias") == 0) {
		if (getassign(&p) || (n = getbool(&p)) < 0)
			goto parse_err;
		config.no_model_alias = !n;
	}
	else if (strcmp(w, "label-alias") == 0) {
		if (getassign(&p) || (n = getbool(&p)) < 0)
			goto parse_err;
		config.no_label_alias = !n;
	}
	else if (strcmp(w, "label-unique") == 0) {
		if (getassign(&p) || (n = getbool(&p)) < 0)
			goto parse_err;
		config.no_label_unique = !n;
	}
	else if (strcmp(w, "uuid-alias") == 0) {
		if (getassign(&p) || (n = getbool(&p)) < 0)
			goto parse_err;
		config.uuid_alias = n;
	}
	else if (strcmp(w, "hide-device-name") == 0) {
		if (getassign(&p) || (n = getbool(&p)) < 0)
			goto parse_err;
		config.hide_device_name = n;
	}
	else if (strcmp(w, "debug") == 0) {
		if (getassign(&p) || (n = getbool(&p)) < 0)
			goto parse_err;
		config.debug = n;
	}
	else if (strcmp(w, "blink-led") == 0) {
		if (getassign(&p) || (n = getled(&p)) < 0)
			goto parse_err;
		config.blink_led = n;
	}
	else if (strcmp(w, "expire-frequency") == 0) {
		if (getassign(&p) || (n = getnum(&p)) < 0)
			goto parse_err;
		if (n < 1) {
			parse_error = "expire-frequency must be > 0";
			goto parse_err;
		}
		config.expire_freq = n;
	}
	else if (strcmp(w, "expire-timeout") == 0) {
		if (getassign(&p) || (n = getnum(&p)) < 0)
			goto parse_err;
		if (n < 1) {
			parse_error = "expire-timeout must be > 0";
			goto parse_err;
		}
		config.expire_timeout = n;
	}
	else if (strcmp(w, "options") == 0) {
		if (!(w = getstr(&p)) || getif(&p) || !(c = getmcondlist(&p)))
			goto parse_err;
		add_fsoptions(c, w);
	}
	else if (strcmp(w, "alias") == 0) {
		if (!(w = getstr(&p)) || getif(&p) || !(c = getmcondlist(&p)))
			goto parse_err;
		add_alias(c, w);
	}
	else if (strcmp(w, "use") == 0) {
		if (!(w = getstr(&p)) ||
			!(w2 = getword(&p)) || strcmp(w2, "instead") != 0 ||
			!(w2 = getstr(&p)))
			goto parse_err;
		add_fstype_replace(w2, w);
	}
	else {
		parse_error = "unknown keyword %s";
		parse_error_arg = w;
		goto parse_err;
	}
	skipwhite(&p);
	if (*p)
		warning(CONFIGFILE ":%d: ignoring junk at end of line", lno);
	return;

  parse_err:
	{
		char where[64], what[512];
		if (p)
			snprintf(where, sizeof(where), " before '%20.20s...'", p);
		else
			*where = '\0';
		snprintf(what, sizeof(what), parse_error, parse_error_arg);
		warning(CONFIGFILE " line %d: parse error%s: %s", lno, where, what);
	}
}

static void purge_config(void)
{
	config = (config_t){ DEF_AUTOFS_EXP_FREQ, DEF_AUTOFS_TIMEOUT,
						 0, 0, 0, 0, 0, 0, 0, 0 };
	purge_fsoptions();
	purge_aliases();
	purge_fstype_replace();
	if (FORCE_DEBUG)
		config.debug = 1;
}

void read_config(void)
{
	struct stat st;
	FILE *f;
	char *line = NULL;
	int len, lno = 0;
	size_t alen;
	
	if (stat(CONFIGFILE, &st))
		return;
	if (st.st_mtime == config_mtime)
		return; /* unchanged */

	if (!(f = fopen(CONFIGFILE, "r"))) {
		error(CONFIGFILE ": %s", strerror(errno));
		return;
	}
	config_mtime = st.st_mtime;
	purge_config();
	while((len = getline(&line, &alen, f)) >= 0) {
		++lno;
		while(len > 0 && strchr(" \t\r\n", line[len-1]))
			line[--len] = '\0';
		if (len && !strchr("#;", line[0])) {
			strpool_init(len);
			parse_line(lno, line);
		}

		xfree((const char**)&line);
	}
	fclose(f);
	strpool_free();
}
