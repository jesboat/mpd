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

void pcm_convertToMpdFixed(AudioFormat * inFormat, char * inBuffer, int 
		samples, char * outBuffer, int fracBits)
{
	mpd_sint8 * in8 = (mpd_sint8 *)inBuffer;
	mpd_sint16 * in16 = (mpd_sint16 *)inBuffer;
	mpd_sint32 * in32 = (mpd_sint32 *)inBuffer;
	mpd_fixed_t * out = (mpd_fixed_t *)outBuffer;
	int shift;

	switch(inFormat->bits) {
	case 8:
		shift = fracBits - 8;
		while(samples--) {
			*out++ = (mpd_fixed_t)(*in8++) << shift;
		}
		break;
	case 16:
		shift = fracBits - 16;
		while(samples--) {
			*out++ = (mpd_fixed_t)(*in16++) << shift;
		}
		break;
	case 32:
		shift = 32 - fracBits;
		while(samples--) {
			*out++ = (mpd_fixed_t)(*in32++) >> shift;
		}
		break; 
	default:
		ERROR("%i bit samples are not supported for conversion!\n",
				inFormat->bits);
		exit(EXIT_FAILURE);
	}
}

/* this is stolen from mpg321! */
inline mpd_uint32 prng(mpd_uint32 state) {
	return (state * 0x0019660dL + 0x3c6ef35fL) & 0xffffffffL;
}
/* end of stolen stuff from mpg321 */

void pcm_convertToIntWithDither(int bits, 
		mpd_fixed_t *buffer, int samples, int fracBits)
{
	static mpd_uint32 ditherRandom = 0;
	mpd_fixed_t mask = ~(~0L << (fracBits - bits));		
	mpd_fixed_t max = (1L << (fracBits)) - 1;
	mpd_fixed_t min = ~0L << (fracBits);
	mpd_fixed_t sample;
	while(samples--) {
		sample = *buffer + (ditherRandom & mask);
		if(sample > max || sample < min)
			ERROR("clipping! %x\n", sample);
		sample = sample>max ? max : (sample<min ? min : sample);
		*buffer = sample >> (fracBits - bits + 1);
		buffer++;
		ditherRandom = prng(ditherRandom);
	}
}


char *pcm_convertSampleRate(AudioFormat *inFormat, char *inBuffer, 
		size_t inFrames, AudioFormat *outFormat, size_t outFrames) 
{
	return NULL;
	/* Input must be float32, 1 or 2 channels */ 
	/* Interpolate using a second order polynomial */
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

/****** exported procedures ***************************************************/

void pcm_changeBufferEndianness(char * buffer, int bufferSize, int bits) {
	
ERROR("pcm_changeBufferEndianess\n");
	switch(bits) {
	case 16:
		while(bufferSize) {
			mpd_uint8 temp = *buffer;
			*buffer = *(buffer+1);
			*(buffer+1) = temp;
			bufferSize-=2;
		}
		break;
	case 32:
		/* I'm not sure if this code is correct */
		/* I guess it is OK for 32 bit int, but how about float? */
		while(bufferSize) {
			mpd_uint8 temp = *buffer;
			mpd_uint8 temp1 = *(buffer+1);
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
	mpd_fixed_t * buffer32 = (mpd_fixed_t *)buffer;
	int iScale;
	int samples;
	int shift; 

	if(format->bits!=32 || format->channels!=2) {
		ERROR("Only 32 bit stereo is supported for pcm_volumeChange!\n");
		exit(EXIT_FAILURE);
	}

	/* take care of full and no volume cases */
	if(volume>=1000) return;
	
	if(volume<=0) {
		memset(buffer,0,bufferSize);
		return;
	}

	/****** change volume ******/
	samples = bufferSize >> 2;
	iScale = (mpd_uint32)(volume * 256) / 1000;
	shift = 8;
	
	/* lower shifting value as much as possible */
	while(!(iScale & 1) && shift) {
		iScale >>= 1;
		shift--;
	}
	/* change */
	if(iScale == 1) {
		while(samples--)
			*buffer32++ = *buffer32 >> shift;
	}
	else {
		while(samples--)
			*buffer32++ = (*buffer32 >> shift) * iScale;
	}	

}

void pcm_add(char * buffer1, char * buffer2, size_t bufferSize1, 
		size_t bufferSize2, int vol1, int vol2, AudioFormat * format)
{
	mpd_fixed_t * buffer32_1 = (mpd_fixed_t *)buffer1;
	mpd_fixed_t * buffer32_2 = (mpd_fixed_t *)buffer2;
	mpd_fixed_t temp;
	int samples1;
	int samples2;
	int iScale1;
	int iScale2;
	int shift;
	
	if(format->bits!=32 || format->channels!=2 ) {
		ERROR("Only 32 bit stereo is supported for pcm_add!\n");
		exit(EXIT_FAILURE);
	}

	samples1 = bufferSize1 >> 2;
	samples2 = bufferSize1 >> 2;
	iScale1 = (mpd_uint32)(vol1 * 256) / 1000;
	iScale2 = (mpd_uint32)(vol2 * 256) / 1000;
	shift = 8;
	
	/* scale and add samples */
	/* no check for overflow needed - we trust our headroom is enough */ 
	while(samples1 && samples2) {
		*buffer32_1++ = (*buffer32_1 >> shift) * iScale1 +
			(*buffer32_2 >> shift) * iScale2;
	}
	/* take care of case where buffer2 > buffer1 */
	if(samples2) memcpy(buffer32_1,buffer32_2,samples2<<2);
	return;

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


void pcm_convertAudioFormat(AudioFormat * inFormat, char * inBuffer, size_t
		inSize, AudioFormat * outFormat, char * outBuffer)
{
	static char *convBuffer = NULL;
	static int convBufferLength = 0;
	char * dataConv;
	int dataLen;
	int fracBits;
	const int inSamples = (inSize << 3) / inFormat->bits;
	const int inFrames = inSamples / inFormat->channels;
	const int outFrames = (inFrames * (mpd_uint32)(outFormat->sampleRate)) / 
			inFormat->sampleRate;
	const int outSamples = outFrames * outFormat->channels;
	
	/* make sure convBuffer is big enough for 2 channels of 32 bit samples */
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

	/* make sure dataConv points to mpd_fixed_t samples */
	if(inFormat->fracBits && inFormat->bits==32) {
		fracBits = inFormat->fracBits;
		dataConv = inBuffer;
	}
	else {
		fracBits = 28; /* use 28 bits as default */
		dataConv = convBuffer;
		pcm_convertToMpdFixed(inFormat, inBuffer, inSamples, 
				dataConv, fracBits);
	}
	
	/****** convert between mono and stereo samples ******/
	
	if(inFormat->channels != outFormat->channels) {
		switch(inFormat->channels) {
		/* convert from 1 -> 2 channels */
		case 1:
			{
			/* in reverse order to allow for same in and out buffer */
			mpd_fixed_t *in = ((mpd_fixed_t *)dataConv)+inFrames;
			mpd_fixed_t *out = ((mpd_fixed_t *)convBuffer)+(inFrames<<1);
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
			mpd_fixed_t *in = ((mpd_fixed_t *)dataConv);
			mpd_fixed_t *out = ((mpd_fixed_t *)convBuffer);
			int f = inFrames;
			while(f--) {
				*out = (*in++)>>1;
				*out++ += (*in++)>>1;
			}
			}
			break;
		default:
			ERROR("only 1 or 2 channels are supported for conversion!\n");
			exit(EXIT_FAILURE);
		}
		dataConv = convBuffer;
	}

	/****** convert sample rate ******/

	if(inFormat->sampleRate != outFormat->sampleRate) {
		dataConv = pcm_convertSampleRate(
				inFormat, dataConv, inFrames, 
				outFormat, outFrames);
	}

	/****** convert to output format ******/

	/* if outformat is mpd_fixed_t then we are done TODO */
	if(outFormat->fracBits) {
		if(outFormat->bits==32) {
			if(outBuffer != dataConv)
				memcpy(outBuffer, dataConv, outSamples << 2);
			return;
		}
		else {
			ERROR("%i bit float are not supported for conversion!\n", 
				outFormat->bits);
			exit(EXIT_FAILURE);
		}
	}
		
	/* convert to regular integer while adding dither and checking range */
	pcm_convertToIntWithDither(outFormat->bits, 
			(mpd_fixed_t *)dataConv, outSamples, fracBits);
	
	/* copy to output buffer*/
	switch(outFormat->bits) {
	case 8:
		{
			mpd_fixed_t *in = (mpd_fixed_t *)dataConv;
			mpd_sint8 * out = (mpd_sint8 *)outBuffer;
			int s = outSamples;
			while(s--) 
				*out++ = *in++;
		}
		break;
	case 16:
		{
			mpd_fixed_t *in = (mpd_fixed_t *)dataConv;
			mpd_sint16 *out = (mpd_sint16 *)outBuffer;
			int s = outSamples;
			while(s--)
				*out++ = *in++;
		}
		break;
	case 32:
		{
			mpd_fixed_t *in = (mpd_fixed_t *)dataConv;
			mpd_sint32 *out = (mpd_sint32 *)outBuffer;
			int s = outSamples;
			while(s--)
				*out++ = *in++;
		}
		break;
	case 24: /* TODO! how do we store 24 bit? */ 
	default:
		ERROR("%i bits are not supported for conversion!\n", outFormat->bits);
		exit(EXIT_FAILURE);
	}

	return;
}

size_t pcm_sizeOfOutputBufferForAudioFormatConversion(AudioFormat * inFormat,
		size_t inSize, AudioFormat * outFormat)
{
	const int inShift = (inFormat->bits * inFormat->channels) >> 3;
	const int outShift = (outFormat->bits * outFormat->channels) >> 3;
	
	size_t inFrames = inSize / inShift;
	size_t outFrames = (inFrames * (mpd_uint32)(outFormat->sampleRate)) / 
			inFormat->sampleRate;
	
	size_t outSize = outFrames * outShift;
	
	return outSize;
}



