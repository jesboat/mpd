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

#include "../config.h"

#include "audioOutput.h"

#ifdef HAVE_SHOUT

#include "conf.h"
#include "log.h"
#include "sig_handlers.h"
#include "pcm_utils.h"

#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <signal.h>

#include <shout/shout.h>
#include <vorbis/vorbisenc.h>
#include <vorbis/codec.h>

static int shoutInitCount = 0;

/* lots of this code blatantly stolent from bossogg/bossao2 */

typedef struct _ShoutData {
	shout_t * shoutConn;

	ogg_stream_state os;
	ogg_page og;
	ogg_packet op;
	ogg_packet header_main;
	ogg_packet header_comments;
	ogg_packet header_codebooks;
	
	vorbis_dsp_state vd;
	vorbis_block vb;
	vorbis_info vi;
	vorbis_comment vc;

	float quality;
	AudioFormat outAudioFormat;
	AudioFormat inAudioFormat;

	char * convBuffer;
	long convBufferLen;
	/* shoud we convert the audio to a different format? */
	int audioFormatConvert;

	int opened;

	MpdTag * tag;
} ShoutData;

static ShoutData * newShoutData() {
	ShoutData * ret = malloc(sizeof(ShoutData));

	ret->shoutConn = shout_new();
	ret->convBuffer = NULL;
	ret->convBufferLen = 0;
	ret->opened = 0;
	ret->tag = NULL;

	return ret;
}

static void freeShoutData(ShoutData * sd) {
	if(sd->shoutConn) shout_free(sd->shoutConn);
	if(sd->tag) freeMpdTag(sd->tag);

	free(sd);
}

#define checkBlockParam(name) { \
	blockParam = getBlockParam(param, name); \
	if(!blockParam) { \
		ERROR("no \"%s\" defined for shout device defined at line " \
				"%i\n", name, param->line); \
		exit(EXIT_FAILURE); \
	} \
}

static int shout_initDriver(AudioOutput * audioOutput, ConfigParam * param) {
	ShoutData * sd;
	char * test;
	int port;
	char * host;
	char * mount;
	char * passwd;
	char * user;
	char * name;
	BlockParam * blockParam;

	sd = newShoutData();

	checkBlockParam("host");
	host = blockParam->value;

	checkBlockParam("mount");
	mount = blockParam->value;

	checkBlockParam("port");

	port = strtol(blockParam->value, &test, 10);

	if(*test != '\0' || port <= 0) {
		ERROR("shout port \"%s\" is not a positive integer, line %i\n", 
				blockParam->value, blockParam->line);
		exit(EXIT_FAILURE);
	}

	checkBlockParam("password");
	passwd = blockParam->value;

	checkBlockParam("name");
	name = blockParam->value;

	checkBlockParam("user");
	user = blockParam->value;

	checkBlockParam("quality");

	sd->quality = strtod(blockParam->value, &test);

	if(*test != '\0' || sd->quality < 0.0 || sd->quality > 10.0) {
		ERROR("shout quality \"%s\" is not a number in the range "
				"0-10, line %i\n", blockParam->value,
				blockParam->line);
		exit(EXIT_FAILURE);
	}

	checkBlockParam("format");

	if(0 != parseAudioConfig(&(sd->outAudioFormat), blockParam->value)) {
		ERROR("error parsing format at line %i\n", blockParam->line);
		exit(EXIT_FAILURE);
	}

	if(shout_set_host(sd->shoutConn, host) !=  SHOUTERR_SUCCESS ||
		shout_set_port(sd->shoutConn, port) != SHOUTERR_SUCCESS ||
		shout_set_password(sd->shoutConn, passwd) != SHOUTERR_SUCCESS ||
		shout_set_mount(sd->shoutConn, mount) != SHOUTERR_SUCCESS ||
		shout_set_name(sd->shoutConn, name) != SHOUTERR_SUCCESS ||
		shout_set_user(sd->shoutConn, user) != SHOUTERR_SUCCESS ||
		shout_set_format(sd->shoutConn, SHOUT_FORMAT_VORBIS) 
			!= SHOUTERR_SUCCESS ||
		shout_set_protocol(sd->shoutConn, SHOUT_PROTOCOL_HTTP)
			!= SHOUTERR_SUCCESS)
	{
		ERROR("error configuring shout: %s\n", 
				shout_get_error(sd->shoutConn));
		exit(EXIT_FAILURE);
	}

	audioOutput->data = sd;

	if(shoutInitCount == 0) shout_init();

	shoutInitCount++;

	return 0;
}

static void clearEncoder(ShoutData * sd) {
	ogg_stream_clear(&(sd->os));
	vorbis_block_clear(&(sd->vb));
	vorbis_dsp_clear(&(sd->vd));
	vorbis_comment_clear(&(sd->vc));
	vorbis_info_clear(&(sd->vi));
}

static void shout_closeShoutConn(ShoutData * sd) {
	if(sd->opened) {
		clearEncoder(sd);

		if(shout_close(sd->shoutConn) != SHOUTERR_SUCCESS) {
			ERROR("problem closing connection to shout server: "
				"%s\n", shout_get_error(sd->shoutConn));
		}
	}

	sd->opened = 0;
}

static void shout_finishDriver(AudioOutput * audioOutput) {
	ShoutData * sd = (ShoutData *)audioOutput->data;

	shout_closeShoutConn(sd);

	freeShoutData(sd);

	shoutInitCount--;

	if(shoutInitCount == 0) shout_shutdown();
}

static void shout_closeDevice(AudioOutput * audioOutput) {
	audioOutput->open = 0;
}

static int shout_handleError(ShoutData * sd, int err) {
	switch(err) {
	case SHOUTERR_SUCCESS:
		break;
	case SHOUTERR_UNCONNECTED:
	case SHOUTERR_SOCKET:
		ERROR("Lost shout connection\n");
		return -1;
	default:
		ERROR("shout: error: %s\n", shout_get_error(sd->shoutConn));
		return -1;
	}

	return 0;
}

static int write_page(ShoutData * sd) {
	shout_sync(sd->shoutConn);
	int err = shout_send(sd->shoutConn, sd->og.header, sd->og.header_len);
	if(shout_handleError(sd, err) < 0) goto fail;
	err = shout_send(sd->shoutConn, sd->og.body, sd->og.body_len);
	if(shout_handleError(sd, err) < 0) goto fail;
	/*shout_sync(sd->shoutConn);*/

	return 0;

fail:
	shout_closeShoutConn(sd);
	return -1;
}

#define addTag(name, value) { \
	if(value) vorbis_comment_add_tag(&(sd->vc), name, value); \
}

static void copyTagToVorbisComment(ShoutData * sd) {
	if(sd->tag) {
		addTag("ARTIST", sd->tag->artist);
		addTag("ALBUM", sd->tag->album);
		addTag("TITLE", sd->tag->title);
	}
}

static int initEncoder(ShoutData * sd) {
	vorbis_info_init(&(sd->vi));

	if( 0 != vorbis_encode_init_vbr(&(sd->vi), sd->outAudioFormat.channels,
			sd->outAudioFormat.sampleRate, sd->quality/10.0) )
	{
		ERROR("problem seting up vorbis encoder for shout\n");
		vorbis_info_clear(&(sd->vi));
		return -1;
	}

	vorbis_analysis_init(&(sd->vd), &(sd->vi));
	vorbis_block_init (&(sd->vd), &(sd->vb));

	ogg_stream_init(&(sd->os), rand());

	vorbis_comment_init(&(sd->vc));

	return 0;
}

static int shout_openShoutConn(AudioOutput * audioOutput) {
	ShoutData * sd = (ShoutData *)audioOutput->data;

	if(shout_open(sd->shoutConn) != SHOUTERR_SUCCESS)
	{
		ERROR("problem opening connection to shout server: %s\n",
				shout_get_error(sd->shoutConn));

		audioOutput->open = 0;
		return -1;
	}

	if(initEncoder(sd) < 0) {
		shout_close(sd->shoutConn);
		audioOutput->open = 1;
		return -1;
	}

	copyTagToVorbisComment(sd);

	vorbis_analysis_headerout(&(sd->vd), &(sd->vc), &(sd->header_main),
			&(sd->header_comments), &(sd->header_codebooks));

	ogg_stream_packetin(&(sd->os), &(sd->header_main));
	ogg_stream_packetin(&(sd->os), &(sd->header_comments));
	ogg_stream_packetin(&(sd->os), &(sd->header_codebooks));

	while(ogg_stream_flush(&(sd->os), &(sd->og)))
	{
		if(write_page(sd) < 0) return -1;
	}

	if(sd->tag) freeMpdTag(sd->tag);
	sd->tag = NULL;

	sd->opened = 1;

	return 0;
}

static int shout_openDevice(AudioOutput * audioOutput,
		AudioFormat * audioFormat) 
{
	ShoutData * sd = (ShoutData *)audioOutput->data;

	memcpy(&(sd->inAudioFormat), audioFormat, sizeof(AudioFormat));

	if(0 == memcmp(&(sd->inAudioFormat), &(sd->outAudioFormat), 
			sizeof(AudioFormat))) 
	{
		sd->audioFormatConvert = 0;
	}
	else sd->audioFormatConvert = 1;

	audioOutput->open = 1;

	if(sd->opened) return 0;

	return shout_openShoutConn(audioOutput);
}

static void shout_convertAudioFormat(ShoutData * sd, char ** chunkArgPtr,
		int * sizeArgPtr)
{
	int size = pcm_sizeOfOutputBufferForAudioFormatConversion(
			&(sd->inAudioFormat), *sizeArgPtr, 
			&(sd->outAudioFormat));

	if(size > sd->convBufferLen) {
		sd->convBuffer = realloc(sd->convBuffer, size);
		sd->convBufferLen = size;
	}

	pcm_convertAudioFormat(&(sd->inAudioFormat), *chunkArgPtr, *sizeArgPtr,
			&(sd->outAudioFormat), sd->convBuffer);
	
	*sizeArgPtr = size;
	*chunkArgPtr = sd->convBuffer;
}

static void shout_sendMetadata(ShoutData * sd) {
	ogg_int64_t granulepos = sd->vd.granulepos;

	if(!sd->opened || !sd->tag) return;

	clearEncoder(sd);
	if(initEncoder(sd) < 0) return;

	sd->vd.granulepos = granulepos;

	copyTagToVorbisComment(sd);

	vorbis_analysis_headerout(&(sd->vd), &(sd->vc), &(sd->header_main),
			&(sd->header_comments), &(sd->header_codebooks));

	ogg_stream_packetin(&(sd->os), &(sd->header_main));
	ogg_stream_packetin(&(sd->os), &(sd->header_comments));
	ogg_stream_packetin(&(sd->os), &(sd->header_codebooks));

	while(ogg_stream_flush(&(sd->os), &(sd->og)))
	{
		if(write_page(sd) < 0) return;
	}

	if(sd->tag) freeMpdTag(sd->tag);
	sd->tag = NULL;
}

static int shout_play(AudioOutput * audioOutput, char * playChunk, int size) {
	int i,j;
	ShoutData * sd = (ShoutData *)audioOutput->data;
	float ** vorbbuf;
	int samples;
	int bytes = sd->outAudioFormat.bits/8;

	if(sd->opened && sd->tag) shout_sendMetadata(sd);

	if(!sd->opened) {
		if(shout_openShoutConn(audioOutput) < 0) {
			return -1;
		}
	}

	if(sd->audioFormatConvert) {
		shout_convertAudioFormat(sd, &playChunk, &size);
	}

	samples = size/(bytes*sd->outAudioFormat.channels);

	/* this is for only 16-bit audio */

	vorbbuf = vorbis_analysis_buffer(&(sd->vd), samples);

	for(i=0; i<samples; i++) {
		for(j=0; j<sd->outAudioFormat.channels; j++) {
			vorbbuf[j][i] = (*((mpd_sint16 *)playChunk)) / 32768.0;
			playChunk += bytes;
		}
	}

	vorbis_analysis_wrote(&(sd->vd), samples);

	while(1 == vorbis_analysis_blockout(&(sd->vd), &(sd->vb))) {
		vorbis_analysis(&(sd->vb), NULL);
		vorbis_bitrate_addblock(&(sd->vb));

		while(vorbis_bitrate_flushpacket(&(sd->vd), &(sd->op))) {
			ogg_stream_packetin(&(sd->os), &(sd->op));
			do {
				if(ogg_stream_pageout(&(sd->os), &(sd->og)) == 0) {
					break;
				}
				if(write_page(sd) < 0) return -1;
			} while(ogg_page_eos(&(sd->og)));
		}
	}

	return 0;
}

static void shout_setTag(AudioOutput * audioOutput, MpdTag * tag) {
	ShoutData * sd = (ShoutData *)audioOutput->data;

	if(sd->tag) freeMpdTag(sd->tag);
	sd->tag = NULL;

	if(!tag) return;

	sd->tag = mpdTagDup(tag);
}

AudioOutputPlugin shoutPlugin = 
{
	"shout",
	shout_initDriver,
	shout_finishDriver,
	shout_openDevice,
	shout_play,
	shout_closeDevice,
	shout_setTag
};

#else

#include <stdlib.h>

AudioOutputPlugin shoutPlugin = 
{
	"shout",
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL
};

#endif
