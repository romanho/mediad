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
#include "mediad.h"


mcond_t *new_mcond(matchwhat_t what, matchop_t op, const char *value)
{
	mcond_t *cond = xmalloc(sizeof(mcond_t));
	
	cond->next  = NULL;
	cond->what  = what;
	cond->op    = op;
	cond->value = xstrdup(value);
	return cond;
}

void free_mcond(mcond_t *p)
{
	mcond_t *next;
	
	for(; p; p = next) {
		next = p->next;
		free((char*)p->value);
		free(p);
	}
}

static int match_one_cond(mcond_t *cond, mnt_t *m, int *fsspec)
{
	int rv;
	
	switch(cond->what) {
	  case MWH_DEVNAME:
	  case MWH_MTABDEVNAME:
		rv = m->dev && strcmp(m->dev, cond->value) == 0;
		break;
	  case MWH_VENDOR:
		rv = m->vendor && strcmp(m->vendor, cond->value) == 0;
		break;
	  case MWH_MODEL:
		rv = m->model && strcmp(m->model, cond->value) == 0;
		break;
	  case MWH_SERIAL:
		rv = m->serial && strcmp(m->serial, cond->value) == 0;
		break;
	  case MWH_PARTITION:
		rv = m->partition == atoi(cond->value);
		break;
	  case MWH_FSTYPE:
		rv = m->type && strcmp(m->type, cond->value) == 0;
		if (fsspec) *fsspec = 1;
		break;
	  case MWH_UUID:
		rv = m->uuid && strcmp(m->uuid, cond->value) == 0;
		if (fsspec) *fsspec = 1;
		break;
	  case MWH_LABEL:
		rv = m->label && strcmp(m->label, cond->value) == 0;
		if (fsspec) *fsspec = 1;
		break;
	  default:
		rv = 0;
	}
	return cond->op == MOP_EQ ? rv : !rv;
}

int match_mcond(mcond_t *cond, mnt_t *m, int *fsspec)
{
	if (fsspec)
		*fsspec = 0;
	for(; cond; cond = cond->next) {
		if (!match_one_cond(cond, m, fsspec))
			return 0;
	}
	return 1;
}

unsigned mcond_prio(mcond_t *cond)
{
	unsigned prio = 999;
	
#define setprio(n) do { if (prio > (n)) prio = (n); } while(0)

	for(; cond; cond = cond->next) {
		switch(cond->what) {
		  case MWH_UUID:		setprio(0); break;
		  case MWH_LABEL:		setprio(1); break;
		  case MWH_SERIAL:		setprio(2); break;
		  case MWH_VENDOR:
		  case MWH_MODEL:		setprio(3); break;
		  case MWH_DEVNAME:		setprio(4); break;
		  case MWH_MTABDEVNAME:	setprio(5); break;
		  case MWH_FSTYPE:		setprio(6); break;
		  default:				break;
		}
	}
	return prio;
}


