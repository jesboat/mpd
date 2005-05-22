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
	float * bufferFloat = (float *)buffer;
	mpd_sint32 * buffer32 = (mpd_sint32 *)buffer;
	mpd_sint16 * buffer16 = (mpd_sint16 *)buffer;
	mpd_sint8 * buffer8 = (mpd_sint8 *)buffer;
	float scale;
	mpd_sint32 iScale;

	if(replayGainState == REPLAYGAIN_OFF || !info) return;

/* DEBUG */
	if(bufferSize % (format->channels * 4))
			ERROR("doReplayGain: bufferSize=%i not multipel of %i\n",
				bufferSize, format->channels);
/* /DEBUG */
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

#ifdef MPD_FIXED_POINT
	if(format->bits!=16 || format.channels!=2 || format->floatSamples) {
		ERROR("Only 16 bit stereo is supported in fixed point mode!\n");
		exit(EXIT_FAILURE);
	}
	
	/* If the volume change is very small then we hardly here the
	 * difference anyway, and if the change is positiv then clipping
	 * may occur. We don't want that. */
	if(info->scale > 0.99) return;

	iScale = scale * 32768.0; /* << 15 */
	
	while(bufferSize) {
		sample32 = (mpd_sint32)(*buffer16) * iScale;
		/* no need to check boundaries - we only lower the volume*/
		/* It would be good to add some kind of dither here... TODO?! */
		*buffer16 = (sample32 >> 15);
		bufferSize -= 2;
	}
	return;
#else

	scale = info->scale;
	
	if(format->floatSamples) {
		if(format->bits==32) {
			while(bufferSize) {
				*bufferFloat *= scale;
				bufferFloat++;
				bufferSize-=4;
			}
			return;
		}
		else {
			ERROR("%i bit float not supported by doReplaygain!\n",
					format->bits);
			exit(EXIT_FAILURE);
		}
	}
	
	switch(format->bits) {
	case 32:
		while(bufferSize) {
			double sample = (double)(*buffer32) * scale;
			if(sample>2147483647.0)	*buffer32 = 2147483647;
			else if(sample<-2147483647.0) *buffer32 = -2147483647;
			else *buffer32 = rintf(sample);
			*buffer32++;
			bufferSize-=4;
		}
		break;
	case 16:
		while(bufferSize){
			float sample = *buffer16 * scale;
			*buffer16 = sample>32767.0 ? 32767 : 
				(sample<-32768.0 ? -32768 : rintf(sample));
			buffer16++;
			bufferSize-=2;
		}
		break;
	case 8:
		while(bufferSize){
			float sample = *buffer8 * scale;
			*buffer8 = sample>127.0 ? 127 : 
				(sample<-128.0 ? -128 : rintf(sample));
			buffer8++;
			bufferSize--;
		}
		break;
	default:
		ERROR("%i bits not supported by doReplaygain!\n", format->bits);
	}
#endif
}
