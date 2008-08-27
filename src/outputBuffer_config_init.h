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

#include "conf.h"
#include "log.h"
#include "utils.h"

#define DEFAULT_BUFFER_SIZE         2048
#define DEFAULT_BUFFER_BEFORE_PLAY  10


void config_output_buffer(void)
{
	float perc = DEFAULT_BUFFER_BEFORE_PLAY;
	char *test;
	size_t buffer_size = DEFAULT_BUFFER_SIZE;
	ConfigParam *param;
	unsigned int buffered_before_play;
	unsigned int buffered_chunks;

	if ((param = getConfigParam(CONF_AUDIO_BUFFER_SIZE))) {
		buffer_size = strtol(param->value, &test, 10);
		if (*test != '\0' || buffer_size <= 0)
			FATAL(CONF_AUDIO_BUFFER_SIZE
			      " \"%s\" is not a positive integer, "
			      "line %i\n", param->value, param->line);
	}

	buffer_size *= 1024;
	buffered_chunks = buffer_size / CHUNK_SIZE;

	if (buffered_chunks >= 1 << 15)
		FATAL("buffer size \"%li\" is too big\n", (long)buffer_size);

	if ((param = getConfigParam(CONF_BUFFER_BEFORE_PLAY))) {
		perc = strtod(param->value, &test);
		if (*test != '%' || perc < 0 || perc > 100)
			FATAL(CONF_BUFFER_BEFORE_PLAY
			      " \"%s\" is not a positive "
			      "percentage and less than 100 percent, line %i"
			      "\n", param->value, param->line);
	}

	buffered_before_play = (perc / 100) * buffered_chunks;
	if (buffered_before_play > buffered_chunks)
		buffered_before_play = buffered_chunks;
	ob.bpp_max = buffered_before_play;

	assert(buffered_chunks > 0 && !ob.index && !ob.chunks);
	ob.index = ringbuf_create(buffered_chunks);
	ob.chunks = xcalloc(ob.index->size, sizeof(struct ob_chunk));
	ob.preseek_len = xmalloc(ob.index->size * sizeof(ob.chunks[0].len));
	ob.state = OB_STATE_STOP;
	ob.sw_vol = 1000;
}

static void ob_free(void)
{
	free(ob.chunks);
	ringbuf_free(ob.index);
}

void init_output_buffer(void)
{
	pthread_attr_t attr;

	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	if (pthread_create(&ob.thread, &attr, ob_task, NULL))
		FATAL("Failed to spawn player task: %s\n", strerror(errno));

	atexit(ob_free);
}
