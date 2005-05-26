/* the Music Player Daemon (MPD)
 * (c)2003-2004 by Warren Dukes (shank@mercury.chem.pitt.edu)
 * (c)2004 replayGain code by AliasMrJones
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

#ifndef REPLAYGAIN_H
#define REPLAYGAIN_H

#include "audio.h"

#define REPLAYGAIN_OFF		0
#define REPLAYGAIN_TRACK	1
#define REPLAYGAIN_ALBUM	2

typedef struct _ReplayGainInfo {
	float albumGain;
	float albumPeak;
	float trackGain;
	float trackPeak;

	/* used internally by mpd, to mess with it*/
	float scale;
	int iScale;
	int shift;
} ReplayGainInfo;

ReplayGainInfo * newReplayGainInfo();

void freeReplayGainInfo(ReplayGainInfo * info);

void initReplayGainState();

void doReplayGain(ReplayGainInfo * info, char * buffer, int bufferSize, 
		AudioFormat * format);

#endif
