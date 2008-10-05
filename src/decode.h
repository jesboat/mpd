/* the Music Player Daemon (MPD)
 * Copyright (C) 2003-2007 by Warren Dukes (warren.dukes@gmail.com)
 * This project's homepage is: http://www.musicpd.org
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#ifndef DECODE_H
#define DECODE_H

#include "song.h"

#include "condition.h"
#include "audio_format.h"
#include "path.h"

#define DECODE_TYPE_FILE	0
#define DECODE_TYPE_URL		1

enum dc_action {
	DC_ACTION_NONE = 0,
	DC_ACTION_START,
	DC_ACTION_SEEK, /* like start, but clears previously decoded audio */
	DC_ACTION_STOP,
	DC_ACTION_QUIT
};

/* only changeable by dc.thread */
enum dc_state {
	DC_STATE_STOP = 0,   /* decoder stopped (no song) */
	DC_STATE_DECODE,     /* open() + {file,stream}DecodeFunc (+ paused) */
	DC_STATE_QUIT        /* NIH, the pthread cancellation API blows... */
};

struct decoder_control {
	char utf8url[MPD_PATH_MAX]; /* only needed for wavpack, remove? */
	enum dc_state state; /* rw=dc.thread, r=main */
	enum dc_action action; /* rw protected by action_cond */
	float total_time;    /* w=dc.thread, r=main */
	float seek_where;    /* -1 == error, rw protected by action_cond */
	pthread_t thread;
	AudioFormat audio_format; /* w=dc.thread, r=all */
};

extern struct decoder_control dc;

void dc_trigger_action(enum dc_action action, float seek_where);
void decoder_init(void);
int dc_try_unhalt(void);
void dc_halt(void);

void dc_action_begin(void);
void dc_action_end(void);

int dc_intr(void);
int dc_seek(void);

enum seek_err_type { DC_SEEK_MISMATCH = -2, DC_SEEK_ERROR = -1 };
void dc_action_seek_fail(enum seek_err_type);

#endif
