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

#ifndef PCM_UTILS_H
#define PCM_UTILS_H

#include "../config.h"

#include "audio_format.h"
#include "os_compat.h"

#ifdef HAVE_LIBSAMPLERATE
#include <samplerate.h>
#endif

typedef struct _ConvState {
#ifdef HAVE_LIBSAMPLERATE
	SRC_STATE *state;
	SRC_DATA data;
	size_t dataInSize;
	size_t dataOutSize;
	mpd_sint8 lastChannels;
	mpd_uint32 lastInSampleRate;
	mpd_uint32 lastOutSampleRate;
#endif
	/* Strict C99 doesn't allow empty structs */
	int error;
} ConvState;

void pcm_volumeChange(char *buffer, int bufferSize, const AudioFormat * format,
                      int volume);

void pcm_mix(char *buffer1, const char *buffer2, size_t bufferSize1,
             size_t bufferSize2, const AudioFormat * format, float portion1);

size_t pcm_convertAudioFormat(const AudioFormat * inFormat,
			      const char *inBuffer, size_t inSize,
			      const AudioFormat * outFormat,
                              char *outBuffer, ConvState *convState);

size_t pcm_sizeOfConvBuffer(const AudioFormat * inFormat, size_t inSize,
                            const AudioFormat * outFormat);
#endif
