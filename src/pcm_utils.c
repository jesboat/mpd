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


#include "pcm_utils.h"

#include "mpd_types.h"
#include "log.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <assert.h>
#include <time.h>

void pcm_changeBufferEndianness(char * buffer, int bufferSize, int bits) {
	
ERROR("pcm_changeBufferEndianess\n");
	switch(bits) {
	case 16:
		while(bufferSize) {
			char temp = *buffer;
			*buffer = *(buffer+1);
			*(buffer+1) = temp;
			bufferSize-=2;
		}
		break;
	case 32:
		/* I'm not sure if this code is correct */
		/* I guess it is OK for 32 bit int, but how about float? */
		while(bufferSize) {
			char temp = *buffer;
			char temp1 = *(buffer+1);
			*buffer = *(buffer+3);
			*(buffer+1) = *(buffer+2);
			*(buffer+2) = temp1;
			*(buffer+3) = temp;
			bufferSize-=4;
		}
		break;
	}
}

void pcm_volumeChange(char * buffer, int bufferSize, AudioFormat * format,
		int volume)
{
#ifdef MPD_FIXED_POINT
	mpd_sint16 * buffer16 = (mpd_sint16 *)buffer;
	mpd_sint32 sample32;

	if(volume>=1000) return;
	
	if(volume<=0) {
		memset(buffer,0,bufferSize);
		return;
	}

	if(format->bits!=16 || format.channels!=2 || format->floatSamples) {
		ERROR("Only 16 bit stereo is supported in fixed point mode!\n");
		exit(EXIT_FAILURE);
	}

	while(bufferSize) {
		sample32 = (mpd_sint32)(*buffer16) * volume;
		/* no need to check boundaries - can't overflow */
		*buffer16 = sample32 >> 10;
		bufferSize -= 2;
	}
	return;
#else
	mpd_sint8 * buffer8 = (mpd_sint8 *)buffer;
	mpd_sint16 * buffer16 = (mpd_sint16 *)buffer;
	mpd_sint32 * buffer32 = (mpd_sint32 *)buffer;
	float * bufferFloat = (float *)buffer;
	float fvolume = volume * 0.001;

	if(volume>=1000) return;
	
	if(volume<=0) {
		memset(buffer,0,bufferSize);
		return;
	}

/* DEBUG */
	if(bufferSize % (format->channels * 4)) {
		ERROR("pcm_volumeChange: bufferSize=%i not multipel of %i\n",
				bufferSize, format->channels * 4);
	}
	if(!format->floatSamples)
		ERROR("pcm_volumeChange: not floatSamples\n");
/* /DEBUG */
	
	if(format->floatSamples) {
		if(format->bits==32) {
			while(bufferSize) {
				*bufferFloat *= fvolume;
				bufferFloat++;
				bufferSize-=4;
			}
			return;
		}
		else {
			ERROR("%i bit float not supported by pcm_volumeChange!\n",
					format->bits);
			exit(EXIT_FAILURE);
		}
	}

	switch(format->bits) {
	case 32: 
		while(bufferSize) {
			double sample = (double)(*buffer32) * fvolume;
			*buffer32++ = rint(sample);
			bufferSize-=4;
		}
		break;
	case 16:
		while(bufferSize) {
			float sample = *buffer16 * fvolume;
			*buffer16++ = rintf(sample);
			bufferSize-=2;
		}
		break;
	case 8:
		while(bufferSize) {
			float sample = *buffer8 * fvolume;
			*buffer8++ = rintf(sample);
			bufferSize--;
		}
		break;
	default:
		ERROR("%i bits not supported by pcm_volumeChange!\n",
				format->bits);
		exit(EXIT_FAILURE);
	}
#endif
}

void pcm_add(char * buffer1, char * buffer2, size_t bufferSize1, 
		size_t bufferSize2, int vol1, int vol2, AudioFormat * format)
{
#ifdef MPD_FIXED_POINT
	mpd_sint16 * buffer16_1 = (mpd_sint16 *)buffer1;
	mpd_sint16 * buffer16_2 = (mpd_sint16 *)buffer2;
	mpd_sint32 sample;
	
	if(format->bits!=16 || format.channels!=2 || format->floatSamples) {
		ERROR("Only 16 bit stereo is supported in fixed point mode!\n");
		exit(EXIT_FAILURE);
	}

	while(bufferSize1 && bufferSize2) {
		sample = ((mpd_sint32)(*buffer16_1) * vol1 +
			(mpd_sint32)(*buffer16_2) * vol2) >> 10;
		*buffer16_1 = sample>32767 ? 32767 : 
			(sample<-32768 ? -32768 : sample); 
		bufferSize1 -= 2;
		bufferSize2 -= 2;
	}
	if(bufferSize2) memcpy(buffer16_1,buffer16_2,bufferSize2);
	return;
#else
/* DEBUG */
	if(bufferSize1 % (format->channels * 4)) {
		ERROR("pcm_add: bufferSize1=%i not multipel of %i\n",
				bufferSize1, format->channels * 4);
	}
	if(bufferSize2 % (format->channels * 4)) {
		ERROR("pcm_add: bufferSize2=%i not multipel of %i\n",
				bufferSize2, format->channels * 4);
	}
	if(!format->floatSamples)
		ERROR("pcm_add: not floatSamples\n");
/* /DEBUG */
	mpd_sint8 * buffer8_1 = (mpd_sint8 *)buffer1;
	mpd_sint8 * buffer8_2 = (mpd_sint8 *)buffer2;
	mpd_sint16 * buffer16_1 = (mpd_sint16 *)buffer1;
	mpd_sint16 * buffer16_2 = (mpd_sint16 *)buffer2;
	mpd_sint32 * buffer32_1 = (mpd_sint32 *)buffer1;
	mpd_sint32 * buffer32_2 = (mpd_sint32 *)buffer2;
	float * bufferFloat_1 = (float *)buffer1;
	float * bufferFloat_2 = (float *)buffer2;
	float fvol1 = vol1 * 0.001;
	float fvol2 = vol2 * 0.001;
	float sample;
	
	if(format->floatSamples) {
		/* 32 bit float */
		while(bufferSize1 && bufferSize2) {
			*bufferFloat_1 = fvol1*(*bufferFloat_1) + 
					fvol2*(*bufferFloat_2);
			bufferFloat_1++;
			bufferFloat_2++;
			bufferSize1-=4;
			bufferSize2-=4;
		}
		if(bufferSize2) memcpy(bufferFloat_1,bufferFloat_2,bufferSize2);
	}
	
	switch(format->bits) {
	case 32:
		while(bufferSize1 && bufferSize2) {
			sample = fvol1*(*buffer32_1) + fvol2*(*buffer32_2);
			if(sample>2147483647.0)	*buffer32_1 = 2147483647;
			else if(sample<-2147483647.0) *buffer32_1 = -2147483647;
			else *buffer32_1 = rintf(sample);
			bufferFloat_1++;
			bufferFloat_2++;
			bufferSize1-=4;
			bufferSize2-=4;
		}
		if(bufferSize2) memcpy(bufferFloat_1,bufferFloat_2,bufferSize2);
		break;
	case 16:
		while(bufferSize1 && bufferSize2) {
			sample = fvol1*(*buffer16_1) + fvol2*(*buffer16_2);
			*buffer16_1 = sample>32767.0 ? 32767 : 
					(sample<-32768.0 ? -32768 : 
					rintf(sample));
			buffer16_1++;
			buffer16_2++;
			bufferSize1-=2;
			bufferSize2-=2;
		}
		if(bufferSize2) memcpy(buffer16_1,buffer16_2,bufferSize2);
		break;
	case 8:
		while(bufferSize1 && bufferSize2) {
			sample = fvol1*(*buffer8_1) + fvol2*(*buffer8_2);
			*buffer8_1 = sample>127.0 ? 127 : 
					(sample<-128.0 ? -128 : 
					rintf(sample));
			buffer8_1++;
			buffer8_2++;
			bufferSize1--;
			bufferSize2--;
		}
		if(bufferSize2) memcpy(buffer8_1,buffer8_2,bufferSize2);
		break;
	default:
		ERROR("%i bits not supported by pcm_add!\n",format->bits);
		exit(EXIT_FAILURE);
	}
#endif
}

void pcm_mix(char * buffer1, char * buffer2, size_t bufferSize1, 
		size_t bufferSize2, AudioFormat * format, float portion1)
{
	int vol1;
	float s = sin(M_PI_2*portion1);
	s*=s;
	
	vol1 = s*1000+0.5;
	vol1 = vol1>1000 ? 1000 : ( vol1<0 ? 0 : vol1 );

	pcm_add(buffer1,buffer2,bufferSize1,bufferSize2,vol1,1000-vol1,format);
}

void pcm_convertToFloat(AudioFormat * inFormat, char * inBuffer, size_t 
		samples, char * outBuffer)
{
	mpd_sint8 * in8 = (mpd_sint8 *)inBuffer;
	mpd_sint16 * in16 = (mpd_sint16 *)inBuffer;
	mpd_sint32 * in32 = (mpd_sint32 *)inBuffer;
	float * out = (float *)outBuffer;
	float multiplier;
	
	switch(inFormat->bits) {
	case 8:
		multiplier = 1.0 / 128.0;
		while(samples--) {
			*out++ = (*in8++) * multiplier;
		}
		break;
	case 16:
		multiplier = 1.0 / 32768.0;
		while(samples--) {
			*out++ = (*in16++) * multiplier;
		}
		break;
/*	case 32:
		multiplier = 1.0 / (1L << 31);
		while(samples--) {
			*out++ = (*in32++) * multiplier;
		}
		break; */
	default:
		ERROR("%i bit samples are not supported for conversion!\n",
				inFormat->bits);
		exit(EXIT_FAILURE);
	}
}


char *pcm_convertSampleRate(AudioFormat *inFormat, char *inBuffer, size_t inFrames, 
		AudioFormat *outFormat, size_t outFrames) 
{
	/* Input must be float32, 1 or 2 channels */ 
	/* Interpolate using a second order polynom */
	/* k0 = s0			*
	 * k2 = (s0 - 2*s1 + s2) * 0.5	*
	 * k1 = s1 - s0 - k2		*
	 * s[t] = k0 + k1*t +k2*t*t	*/

	static float * sampleConvBuffer = NULL;
	static int sampleConvBufferLength = 0;
	size_t dataSampleLen = 0;
	
	float *out;
	float *in = (float *)inBuffer;
	
	static float shift;
	static float offset;
	static float sample0l;
	static float sample0r;
	static float sample1l;
	static float sample1r;

	static int rateCheck = 0;
	static time_t timeCheck = 0;
	size_t c_rate = inFormat->sampleRate + inFormat->channels;
	time_t c_time = time(NULL);
	
	/* reset static data if changed samplerate ...*/
	if(c_rate != rateCheck || c_time != timeCheck) {
		ERROR("reset resampling\n",c_rate, rateCheck);
		rateCheck = c_rate;
		shift = (float)inFrames / (float)outFrames;
		offset = 1.5;
		sample0l = 0.0;
		sample0r = 0.0;
		sample1l = 0.0;
		sample1r = 0.0;
	}
	else { 
		/* ... otherwise check that shift is within bounds */ 
		float s = offset + (outFrames * shift) - inFrames;
		if(s > 1.5) {
			shift = (1.5-offset+(float)inFrames) / (float)outFrames;
		}
		else if(s < 0.5) {
			shift = (0.5-offset+(float)inFrames) / (float)outFrames;
		}
	}
	timeCheck = c_time;

	/* allocate data */
	dataSampleLen = 8 * outFrames;
	if(dataSampleLen > sampleConvBufferLength) {
		sampleConvBuffer = (float *)realloc(sampleConvBuffer,dataSampleLen);
		sampleConvBufferLength = dataSampleLen;
	}
	out = sampleConvBuffer;
	

	/* convert */
	switch(outFormat->channels) {
	case 1:	
	{
		float sample2l;
		float k0l, k1l, k2l;
		while(inFrames--) {
			sample2l = *in++;
			/* set coefficients */
			k0l = sample0l;
			k2l = (sample0l - 2.0 * sample1l + sample2l) * 0.5;
			k1l = sample1l - sample0l - k2l;
			/* calculate sample(s) */
			while(offset <= 1.5 && outFrames--) {
				*out++ = k0l + k1l*offset + k2l*offset*offset;
				offset += shift;
			}
			/* prepare for next frame */
			sample0l = sample1l;
			sample1l = sample2l;
			offset -= 1.0;
		}
	
		/* fill the last frames */
		while(outFrames--) {
			*out++ = k0l + k1l*offset + k2l*offset*offset;
			offset += shift;
		}
	}		
	break;
	case 2:	
	{
		float *out = sampleConvBuffer;
		float *in = (float *)inBuffer;
		float sample2l;
		float sample2r;
		float k0l, k1l, k2l;
		float k0r, k1r, k2r;
		while(inFrames--) {
			sample2l = *in++;
			sample2r = *in++;
			/* set coefficients */
			k0l = sample0l;
			k0r = sample0r;
			k2l = (sample0l - 2.0 * sample1l + sample2l) * 0.5;
			k2r = (sample0r - 2.0 * sample1r + sample2r) * 0.5;
			k1l = sample1l - sample0l - k2l;
			k1r = sample1r - sample0r - k2r;
			/* calculate sample(s) */
			while(offset <= 1.5 && outFrames--) {
			if(offset<0.5)
				ERROR("offset to small in resampling - %f\n",offset);
				*out++ = k0l + k1l*offset + k2l*offset*offset;
				*out++ = k0r + k1r*offset + k2r*offset*offset;
				offset += shift;
			}
			/* prepare for next frame */
			sample0l = sample1l;
			sample0r = sample1r;
			sample1l = sample2l;
			sample1r = sample2r;
			offset -= 1.0;
		}
	
		/* fill the last frame(s) */
		while(outFrames--) {
			if(offset>2.0)
				ERROR("offset to big in resampling - %f\n",offset);
			*out++ = k0l + k1l*offset + k2l*offset*offset;
			*out++ = k0r+ k1r*offset + k2r*offset*offset;
			offset += shift;
		}
	}		
	break;
	}
	return (char *)sampleConvBuffer;
}


/* now support conversions between sint8, sint16, sint32 and float32, 
 * 1 or 2 channels and (although badly) all sample rates */
void pcm_convertAudioFormat(AudioFormat * inFormat, char * inBuffer, size_t
		inSize, AudioFormat * outFormat, char * outBuffer)
{
#ifdef MPD_FIXED_POINT
	/* In fixed mode the conversion support is limited... */ 
	if(inFormat->bits != outFormat->bits || inFormat->bits != 16) {
		ERROR("Only 16 bit samples supported in fixed point mode!\n");
		exit(EXIT_FAILURE);
	}
	if(inFormat->sampleRate || outFormat->sampleRate) {
		ERROR("Sample rate conversions not supported in fixed point mode!\n");
		exit(EXIT_FAILURE);
	}
	if(inFormat->channels == 2 && outFormat->channels == 1) {
		size_t frames = inSize >> 2; /* 16 bit stereo == 4 bytes */
		mpd_sint16 *in = (mpd_sint16 *)inBuffer;
		mpd_sint16 *out = (mpd_sint16 *)outBuffer;
		while(frames--) {
			*out++ = *in++;
			*in++; /* skip the other channel */
		}
	}
	if(inFormat->channels == 1 && outFormat->channels == 2) {
		size_t frames = inSize >> 1; /* 16 bit mono == 2 bytes */
		mpd_sint16 *in = (mpd_sint16 *)inBuffer;
		mpd_sint16 *out = (mpd_sint16 *)outBuffer;
		while(frames--) {
			*out++ = *in;
			*out++ = *in++; /* duplicate the channel */
		}
	}
	else {
		ERROR("More then 2 channels are not supported!\n");
		exit(EXIT_FAILURE);
	}
	return;
	
#else 
/* DEBUG */
	if(inSize % (inFormat->channels * 4)) {
		ERROR("pcm_convertAudioFormat: inSize=%i not multipel of %i\n",
				inSize, inFormat->channels * 4);
	}
/* /DEBUG */

	static char *convBuffer = NULL;
	static int convBufferLength = 0;
	char * dataConv;
	int dataLen;
	static float ditherAmount = 2.0 / RAND_MAX;
	const size_t inSamples = (inSize << 3) / inFormat->bits;
	const size_t inFrames = inSamples / inFormat->channels;
	const size_t outFrames = (inFrames * (mpd_uint32)(outFormat->sampleRate)) / 
			inFormat->sampleRate;
	const size_t outSamples = outFrames * outFormat->channels;
		
	/* make sure convBuffer is big enough for 2 channels of float samples */
	dataLen = inFrames << 3;
	if(dataLen > convBufferLength) {
		convBuffer = (char *) realloc(convBuffer, dataLen);
		if(!convBuffer)
		{
			ERROR("Could not allocate more memory for convBuffer!\n");
			exit(EXIT_FAILURE);
		}
		convBufferLength = dataLen;
	}

	/* make sure dataConv points to 32 bit float samples */
	if(inFormat->floatSamples && inFormat->bits==32) {
		dataConv = inBuffer;
	}
	else if(!inFormat->floatSamples) {
		dataConv = convBuffer;
		pcm_convertToFloat(inFormat, inBuffer, inSamples, dataConv);
	}
	else {
		ERROR("%i bit float are not supported for conversion!\n", 
				inFormat->bits);
		exit(EXIT_FAILURE);
	}
	
	/* convert between mono and stereo samples*/
	if(inFormat->channels != outFormat->channels) {
		float *in = ((float *)dataConv)+inFrames;
		switch(inFormat->channels) {
		/* convert from 1 -> 2 channels */
		case 1:
			{
			float *out = ((float *)convBuffer)+(inFrames<<1);
			int f = inFrames;
			while(f--) {
				*out-- = *in;
				*out-- = *in--;
			}
			}
			break;
		/* convert from 2 -> 1 channels */
		case 2:
			{
			float * out = (float *)convBuffer;
			int f = inFrames;
			while(f--) {
				*out = (*in++)*0.5;
				*out++ += (*in++)*0.5;
			}
			}
			break;
		default:
			ERROR("only 1 or 2 channels are supported for conversion!\n");
			exit(EXIT_FAILURE);
		}
		dataConv = convBuffer;
	}

	/* convert sample rate */
	if(inFormat->sampleRate != outFormat->sampleRate) {
		dataConv = pcm_convertSampleRate(
				inFormat, dataConv, inFrames, 
				outFormat, outFrames);
	}

	/* convert to output format */
	if(outFormat->floatSamples && outFormat->bits==32) {
		if(outBuffer != dataConv)
			memcpy(outBuffer, dataConv, outSamples << 2);
		return;
	}
	else if(outFormat->floatSamples) {
		ERROR("%i bit float are not supported for conversion!\n", 
				outFormat->bits);
		exit(EXIT_FAILURE);
	}
		
	switch(outFormat->bits) {
	case 8:
		{
		/* add triangular dither and convert to sint8 */
		float * in = (float *)dataConv;
		mpd_sint8 * out = (mpd_sint8 *)outBuffer;
		int s = outSamples;
		while(s--) {
			float sample = (*in++) * 128.0 + 
				ditherAmount*(rand()-rand());
			*out++ = sample>127.0 ? 127 :
				(sample<-128.0 ? -128 : 
				 rintf(sample));
		}
		}
		break;
	case 16:
		{
		/* add triangular dither and convert to sint16 */
		float * in = (float *)dataConv;
		mpd_sint16 * out = (mpd_sint16 *)outBuffer;
		int s = outSamples;
		while(s--) {
			float sample = (*in++) * 32766.0 + 
				ditherAmount*(rand()-rand());
			*out++ = sample>32767.0 ? 32767 :
				(sample<-32768.0 ? -32768 :
				 rintf(sample));
		}
		}
		break;
	case 32:
		{
		/* convert to sint32 */
		float * in = (float *)dataConv;
		mpd_sint32 * out = (mpd_sint32 *)outBuffer;
		int s = outSamples;
		while(s--) {
			float sample = (*in++) * 2147483647.0;
			if(sample>2147483647.0)	*out++ = 2147483647;
			else if(sample<-2147483647.0) *out++ = -2147483647;
			else *out++ = rintf(sample);
		}
		}
		break;
	case 24: /* how do we store 24 bit? Maybe sint32 is all we need? */ 
	default:
		ERROR("%i bits are not supported for conversion!\n", outFormat->bits);
		exit(EXIT_FAILURE);
	}

	return;
#endif
}

size_t pcm_sizeOfOutputBufferForAudioFormatConversion(AudioFormat * inFormat,
		size_t inSize, AudioFormat * outFormat)
{
/* DEBUG */
	if(inSize % (inFormat->channels * 4)) {
		ERROR("pcm_sizeOOBFAFC: inSize=%i not multipel of %i\n",
				inSize, inFormat->channels * 4);
	}
/* /DEBUG */
	const int inShift = (inFormat->bits * inFormat->channels) >> 3;
	const int outShift = (outFormat->bits * outFormat->channels) >> 3;
	
	size_t inFrames = inSize / inShift;
	size_t outFrames = (inFrames * (mpd_uint32)(outFormat->sampleRate)) / 
			inFormat->sampleRate;
	
	size_t outSize = outFrames * outShift;
	
	return outSize;
}
