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
	static mpd_uint32 ditherRandom[2] = {0,0};
	const mpd_fixed_t mask = ~(~0L << (fracBits - bits));
	const mpd_fixed_t half = 1L << (fracBits - bits - 1);
	const mpd_fixed_t max = (1L << (fracBits)) - 1;
	const mpd_fixed_t min = ~0L << (fracBits);
	mpd_fixed_t sample;
	
	/* need to split in two cases to avoid negative shifting */
	if(bits>fracBits) {
		/* left shift - no need to dither */
		while(samples--) {
			sample = *buffer;
			if(sample>max)
				sample = max;
			else if(sample<min) 
				sample = min;
			*buffer++ = sample << (bits - fracBits - 1);
		}
	}
	else {
		/* right shift - add 1 bit triangular dither */
		while(samples--) {
			sample = *buffer + half + (ditherRandom[0] & mask) -
				(ditherRandom[1] & mask);
			if(sample>max)
				sample = max;
			else if(sample<min)
				sample = min;
			*buffer++ = sample >> (fracBits - bits + 1);
			ditherRandom[1] = ditherRandom[0] >> 1;
			ditherRandom[0] = prng(ditherRandom[0]);
		}
	}
}

struct {
	mpd_uint32 delay;
	mpd_uint32 inRate;
	mpd_uint32 outRate;
} convSampleRateData = {0,0,0};

void pcm_convertSampleRate(
		AudioFormat * inFormat, mpd_fixed_t * inBuffer, int inFrames, 
		AudioFormat *outFormat, mpd_fixed_t *outBuffer, int outFrames) 
{
	static int inRate;
	static int outRate;
	static int shift;
	static int rateShift;
	static mpd_fixed_t oldSampleL = 0;
	static mpd_fixed_t oldSampleR = 0;
	int delay;
	
	/* recalculate static data if samplerate has changed */
	if(inFormat->sampleRate != convSampleRateData.inRate || 
			outFormat->sampleRate != convSampleRateData.outRate) {
		/* set new sample rate info and reset delay */
		convSampleRateData.inRate = inFormat->sampleRate;
		convSampleRateData.outRate = outFormat->sampleRate;
		convSampleRateData.delay = 0;
		/* calculate the rates to use in calculations... */
		inRate = inFormat->sampleRate;
		outRate = outFormat->sampleRate;
		rateShift=0;
		shift = 16; /* worst case shift */
		/* ...reduce them to minimize shifting */
		while(!(inRate & 1) && !(outRate & 1)) {
			rateShift++;
			shift--;
			inRate >>= 1;
			outRate >>= 1;
		}
		oldSampleL = 0;
		oldSampleR = 0;
	}

	/* compute */
	
	delay = convSampleRateData.delay >> rateShift;
	switch(inFormat->channels) {
	case 1:
		while(inFrames--) {
			delay += outRate;
			/* calculate new samples */
			while(delay >= inRate) {
				mpd_sint32 raise;
				delay -= inRate;
				raise = *inBuffer - oldSampleL;
				*outBuffer++ = oldSampleL + 
					((((raise>>shift) * (outRate - delay)) / 
					 outRate) << shift);
			}
			oldSampleL = *inBuffer++;
		}
		break;
	case 2:
		while(inFrames--) {
			delay += outRate;
			/* calculate new samples */
			while(delay >= inRate) {
				mpd_sint32 raise;
				delay -= inRate;
				/* left channel */
				raise = *inBuffer - oldSampleL;
				*outBuffer++ = oldSampleL + 
					((((raise>>shift) * (outRate - delay)) / 
					 outRate) << shift);
				/* right channel */
				raise = inBuffer[1] - oldSampleR;
				*outBuffer++ = oldSampleR + 
					((((raise>>shift) * (outRate - delay)) /
					 outRate) << shift);
			}
			oldSampleL = *inBuffer++;
			oldSampleR = *inBuffer++;
		}
		/* exit(EXIT_FAILURE);*/
		break;
	}

	convSampleRateData.delay = delay << rateShift;
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
	int samples = bufferSize >> 2;
	static int iScale;
	static int shift;
	static int currentVolume = -1;

	if(format->bits!=32 || format->fracBits == 0) {
		ERROR("Only 32 bit mpd_fixed_t samples are supported in"
				" pcm_volumeChange!\n");
		exit(EXIT_FAILURE);
	}

	/* take care of full and no volume cases */
	if(volume>=1024) return;
	
	if(volume<=0) {
		memset(buffer,0,bufferSize);
		return;
	}

	/* recalculate if volume has changed */
	if(volume != currentVolume) {
		currentVolume = volume;
		iScale = volume;
		shift = 10;
		
		/* Minimize values to get the precision loss as small as 
		 * possible in the integer calculations. Make iScale less
		 * then 5 bits. This results in a volume change precision
		 * of approx. 0.5dB */
		while((iScale>31 || !(iScale & 1)) && shift) {
			iScale >>= 1;
			shift--;
		}
	}

	/* change the volume */
	if(iScale == 1)
		while(samples--) {
			*buffer32 = *buffer32 >> shift;
			buffer32++;
		}
	else 
		while(samples--) {
			*buffer32 = (*buffer32 >> shift) * iScale;
			buffer32++;
		}
}

void pcm_add(char * buffer1, char * buffer2, size_t bufferSize1, 
		size_t bufferSize2, int vol1, int vol2, AudioFormat * format)
{
	mpd_fixed_t * buffer32_1 = (mpd_fixed_t *)buffer1;
	mpd_fixed_t * buffer32_2 = (mpd_fixed_t *)buffer2;
	mpd_fixed_t temp;
	int samples1 = bufferSize1 >> 2;
	int samples2 = bufferSize2 >> 2;
	int shift = 10;
	
	if(format->bits!=32 || format->fracBits==0 ) {
		ERROR("Only 32 bit mpd_fixed_t samples are supported in"
				" pcm_add!\n");
		exit(EXIT_FAILURE);
	}
	
	/* take care of zero volume cases */
	if(vol2<=0) {
		return;
	}
	if(vol1<=0) {
		if(bufferSize1>bufferSize2) {
			memcpy(buffer1, buffer2, bufferSize2);
			memset(buffer1+bufferSize2, 0, bufferSize1-bufferSize2);
		}
		else {
			memcpy(buffer1, buffer2, bufferSize1);
		}		
		return;
	}

	/* lower multiplicator to minimize audio resolution loss */
	while((vol1>31 || !(vol1 & 1)) && (vol2>31 || !(vol2 & 1)) && shift) {
		vol1 >>= 1;
		vol2 >>= 1;
		shift--;
	}
			
	/* scale and add samples */
	/* no check for overflow needed - we trust our headroom is enough */ 
	while(samples1-- && samples2--) {
		temp = (*buffer32_1 >> shift) * vol1 +
			(*buffer32_2 >> shift) * vol2;
		*buffer32_1 = temp;
		buffer32_1++;
		buffer32_2++;
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
	
	vol1 = s*1024+0.5;
	vol1 = vol1>1024 ? 1024 : ( vol1<0 ? 0 : vol1 );

	pcm_add(buffer1,buffer2,bufferSize1,bufferSize2,vol1,1024-vol1,format);
}


void pcm_convertAudioFormat(AudioFormat * inFormat, char * inBuffer, size_t
		inSize, AudioFormat * outFormat, char * outBuffer)
{
	static char *convBuffer = NULL;
	static int convBufferLength = 0;
	static char *convSampleBuffer = NULL;
	static int convSampleBufferLength = 0;
	char * dataConv;
	int dataLen;
	int fracBits;
	const int inSamples = (inSize << 3) / inFormat->bits;
	const int inFrames = inSamples / inFormat->channels;
	const size_t outSize = pcm_sizeOfOutputBufferForAudioFormatConversion(
			inFormat, inSize, outFormat);
	const int outSamples = (outSize << 3) / outFormat->bits;
	const int outFrames = outSamples / outFormat->channels;
	
	/* make sure convBuffer is big enough for 2 channels of 32 bit samples */
	dataLen = inFrames << 3;
	if(dataLen > convBufferLength) {
		convBuffer = (char *) realloc(convBuffer, dataLen);
		if(!convBuffer) {
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
	
	/****** convert sample rate ******/

	if(inFormat->sampleRate != outFormat->sampleRate) {
		/* check size of buffer */
		dataLen = outFrames << 3;
		if(dataLen > convSampleBufferLength) {
			convSampleBuffer = (char *) 
				realloc(convSampleBuffer, dataLen);
			if(!convSampleBuffer) {
				ERROR("Could not allocate memory for " 
						"convSampleBuffer!\n");
				exit(EXIT_FAILURE);
			}
			convSampleBufferLength = dataLen;
		}
		/* convert samples */
		pcm_convertSampleRate(inFormat, (mpd_fixed_t*)dataConv, inFrames, 
			outFormat, (mpd_fixed_t*)convSampleBuffer, outFrames);
		dataConv = convSampleBuffer;
	}

	/****** convert between mono and stereo samples ******/
	
	if(inFormat->channels != outFormat->channels) {
		switch(inFormat->channels) {
		/* convert from 1 -> 2 channels */
		case 1:
			{
			/* in reverse order to allow for same in and out buffer */
			mpd_fixed_t *in = ((mpd_fixed_t *)dataConv) + outFrames - 1;
			mpd_fixed_t *out = ((mpd_fixed_t *)convBuffer) + outSamples - 1;
			int f = outFrames;
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
			int f = outFrames;
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

	/****** convert to output format ******/

	/* if outformat is mpd_fixed_t then we are done ?! 
	 * TODO take care of case when in and out have different fracBits */
	if(outFormat->fracBits==fracBits) {
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
	const int inFrames = inSize / inShift;
	mpd_uint32 outFrames;
	
	if(inFormat->sampleRate == outFormat->sampleRate)
		outFrames = inFrames;
	else {
		/* The previous delay from the sample rate conversion affect 
		 * the size of the output */
		mpd_uint32 delay = convSampleRateData.delay;
		if(inFormat->sampleRate != convSampleRateData.inRate || 
			outFormat->sampleRate != convSampleRateData.outRate) 
		{
			delay = 0;
		}
		outFrames = (inFrames * (mpd_uint32)(outFormat->sampleRate) 
				+ delay) / inFormat->sampleRate;
	}
	return outFrames * outShift;
}



