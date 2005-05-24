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

#include "replayGain.h"

#include "log.h"
#include "conf.h"

#include <string.h>
#include <math.h>
#include <stdlib.h>
 
/* Added 4/14/2004 by AliasMrJones */
static int replayGainState = REPLAYGAIN_OFF;

static float replayGainPreamp = 1.0;

void initReplayGainState() {
	ConfigParam * param = getConfigParam(CONF_REPLAYGAIN);

	if(!param) return;

	if(strcmp(param->value, "track") == 0) {
		replayGainState = REPLAYGAIN_TRACK;
	}
	else if(strcmp(param->value, "album") == 0) {
		replayGainState = REPLAYGAIN_ALBUM;
	}
	else {
		ERROR("replaygain value \"%s\" at line %i is invalid\n",
				param->value, param->line);
		exit(EXIT_FAILURE);
	}

	param = getConfigParam(CONF_REPLAYGAIN_PREAMP);

	if(param) {
		char * test;
		float f = strtod(param->value, &test);

		if(*test != '\0') {
			ERROR("Replaygain preamp \"%s\" is not a number at "
					"line %i\n", param->value, param->line);
			exit(EXIT_FAILURE);
		}

		if(f < -15 || f > 15) {
			ERROR("Replaygain preamp \"%s\" is not between -15 and"
					"15 at line %i\n", 
					param->value, param->line);
			exit(EXIT_FAILURE);
		}

		replayGainPreamp = pow(10, f/20.0);
	}
}

static float computeReplayGainScale(float gain, float peak) {
	float scale;

	if(gain == 0.0) return(1);
	scale = pow(10.0, gain/20.0);
	scale*= replayGainPreamp;
	if(scale > 15.0) scale = 15.0;

	if (scale * peak > 1.0) {
		scale = 1.0 / peak;
	}
	return(scale);
}

ReplayGainInfo * newReplayGainInfo() {
	ReplayGainInfo * ret = malloc(sizeof(ReplayGainInfo));

	ret->albumGain = 0.0;
	ret->albumPeak = 0.0;

	ret->trackGain = 0.0;
	ret->trackPeak = 0.0;

	/* set to -1 so that we know in doReplayGain to compute the scale */
	ret->scale = -1.0;

	return ret;
}

void freeReplayGainInfo(ReplayGainInfo * info) {
	free(info);
}

void doReplayGain(ReplayGainInfo * info, char * buffer, int bufferSize, 
		AudioFormat * format)
{
	mpd_sint32 * buffer32 = (mpd_sint32 *)buffer;
	int samples;
	int shift; 
	int iScale;

	if(format->bits!=32 || format->channels!=2 ) {
		ERROR("Only 32 bit stereo is supported for doReplayGain!\n");
		exit(EXIT_FAILURE);
		return;
	}
	
	if(replayGainState == REPLAYGAIN_OFF || !info) return;

	if(info->scale < 0) {
		switch(replayGainState) {
		case REPLAYGAIN_TRACK:
			info->scale = computeReplayGainScale(info->trackGain,
							info->trackPeak);
			break;
		default:
			info->scale = computeReplayGainScale(info->albumGain,
							info->albumPeak);
			break;
		}
	}
	
	samples = bufferSize >> 2;
	iScale = info->scale * 256; 
	shift = 8;

	
	/* handle negative or zero scale */
	if(iScale<=0) {
		memset(buffer,0,bufferSize);
		return;
	}
	
	/* lower shift value as much as possible */
	while(!(iScale & 1) && shift) {
		iScale >>= 1;
		shift--;
	}
	
	/* change samples */
	/* no check for overflow needed - replaygain peak info prevent
	 * clipping and we have 3 headroom bits in our 32 bit samples */ 
	if(iScale == 1) {
		while(samples--)
			*buffer32++ = *buffer32 >> shift;
	}
	else {
		while(samples--)
			*buffer32++ = (*buffer32 >> shift) * iScale;
	}	

}
