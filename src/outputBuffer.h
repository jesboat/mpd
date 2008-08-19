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

#ifndef OUTPUT_BUFFER_H
#define OUTPUT_BUFFER_H

#include "pcm_utils.h"
#include "mpd_types.h"
#include "decode.h"
#include "inputStream.h"
#include "replayGain.h"

/*
 * As far as audio output is concerned, `stop' is a superset of `pause'
 * That is, `stop' will drop decoded audio chunks from the buffer
 * and `pause' will not.  Both will stop audio playback immediately
 * and close audio playback devices (TODO: close mixer devices).
 */
enum ob_action {
	OB_ACTION_NONE = 0,
	OB_ACTION_PLAY,
	OB_ACTION_DROP,
	OB_ACTION_SEEK_START,
	OB_ACTION_SEEK_FINISH,
	OB_ACTION_PAUSE_SET,
	OB_ACTION_PAUSE_UNSET,
	OB_ACTION_PAUSE_FLIP,
	OB_ACTION_RESET,
	OB_ACTION_STOP,
	OB_ACTION_QUIT
};

/* 1020 bytes since its divisible for 8, 16, 24, and 32-bit audio */
#define CHUNK_SIZE		1020

void ob_flush(void);

enum ob_drop_type { OB_DROP_DECODED, OB_DROP_PLAYING };
void ob_drop_audio(enum ob_drop_type type);

/*
 * Returns true if output buffer is playing the song we're decoding
 */
int ob_synced(void);

/*
 * analogous to send(2) or write(2), it will put @data into
 * the output buffer (like writing to a pipe, the consumer of
 * which will read and play the contents of the output buffer
 *
 * Future direction:
 *   zero-copy functions using vectors:
 *   	vec = ob_getv(); decode_to(&vec, ...); ob_vmsplice(&vec, ...);
 */
enum dc_action ob_send(void *data, size_t len, float time,
                       mpd_uint16 bit_rate, ReplayGainInfo *rgi);

/* synchronous and blocking (the only way it should be) */
void ob_trigger_action(enum ob_action action);

/* synchronous and blocking, called from dc.thread */
void ob_seek_start(void);
void ob_seek_finish(void);

/* boring accessor functions, only called by main-thread */
unsigned long ob_get_elapsed_time(void);
unsigned long ob_get_total_time(void);
unsigned int ob_get_channels(void);
unsigned int ob_get_bit_rate(void);
unsigned int ob_get_sample_rate(void);
unsigned int ob_get_bits(void);
void ob_set_sw_volume(int volume);
void ob_set_xfade(float xfade_seconds);
float ob_get_xfade(void);

enum ob_state {
	OB_STATE_PLAY = 0,
	OB_STATE_STOP,
	OB_STATE_PAUSE,
	OB_STATE_SEEK,
	OB_STATE_QUIT
};

enum ob_state ob_get_state(void);

AudioFormat *ob_audio_format(void);

void ob_advance_sequence(void);

void ob_flush(void);

void config_output_buffer(void);
void init_output_buffer(void);

#endif
