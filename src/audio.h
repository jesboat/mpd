/* the Music Player Daemon (MPD)
 * (c)2003-2004 by Warren Dukes (shank@mercury.chem.pitt.edu)
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

#ifndef AUDIO_H
#define AUDIO_H

#include "../config.h"

#include "mpd_types.h"
#include "tag.h"

#include <stdio.h>

#define AUDIO_AO_DRIVER_DEFAULT	"default"

#define AUDIO_MAX_DEVICES	8

typedef struct _AudioFormat {
	volatile mpd_sint8 channels;
	volatile mpd_uint32 sampleRate;
	volatile mpd_sint8 bits;
} AudioFormat;

void copyAudioFormat(AudioFormat * dest, AudioFormat * src);

int cmpAudioFormat(AudioFormat * dest, AudioFormat * src);

void getOutputAudioFormat(AudioFormat * inFormat, AudioFormat * outFormat);

int parseAudioConfig(AudioFormat * audioFormat, char * conf);

/* make sure initPlayerData is called before this function!! */
void initAudioConfig();

void finishAudioConfig();

void initAudioDriver();

void finishAudioDriver();

int openAudioDevice(AudioFormat * audioFormat);

int playAudio(char * playChunk,int size);

void dropBufferedAudio();

void closeAudioDevice();

int isAudioDeviceOpen();

int isCurrentAudioFormat(AudioFormat * audioFormat);

void sendMetadataToAudioDevice(MpdTag * tag);

/* these functions are called in the main parent process while the child
	process is busy playing to the audio */
int enableAudioDevice(FILE * fp, int device);

int disableAudioDevice(FILE * fp, int device);

void printAudioDevices(FILE * fp);

void readAudioDevicesState();

#endif
