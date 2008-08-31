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

/* TODO 'ogg' should probably be replaced with 'oggvorbis' in all instances */

#include "../inputPlugin.h"

#ifdef HAVE_OGGVORBIS

#include "_ogg_common.h"

#include "../utils.h"
#include "../audio.h"
#include "../log.h"
#include "../pcm_utils.h"
#include "../inputStream.h"
#include "../outputBuffer.h"
#include "../replayGain.h"
#include "../os_compat.h"

#ifndef HAVE_TREMOR
#include <vorbis/vorbisfile.h>
#else
#include <tremor/ivorbisfile.h>
/* Macros to make Tremor's API look like libogg. Tremor always
   returns host-byte-order 16-bit signed data, and uses integer
   milliseconds where libogg uses double seconds. 
*/
#define ov_read(VF, BUFFER, LENGTH, BIGENDIANP, WORD, SGNED, BITSTREAM) \
        ov_read(VF, BUFFER, LENGTH, BITSTREAM)
#define ov_time_total(VF, I) ((double)ov_time_total(VF, I)/1000)
#define ov_time_tell(VF) ((double)ov_time_tell(VF)/1000)
#define ov_time_seek_page(VF, S) (ov_time_seek_page(VF, (S)*1000))
#endif /* HAVE_TREMOR */

#ifdef WORDS_BIGENDIAN
#define OGG_DECODE_USE_BIGENDIAN	1
#else
#define OGG_DECODE_USE_BIGENDIAN	0
#endif

typedef struct _OggCallbackData {
	InputStream *inStream;
} OggCallbackData;

static size_t ogg_read_cb(void *ptr, size_t size, size_t nmemb, void *vdata)
{
	size_t ret;
	OggCallbackData *data = (OggCallbackData *) vdata;

	while (1) {
		ret = readFromInputStream(data->inStream, ptr, size, nmemb);
		if (ret == 0 && !inputStreamAtEOF(data->inStream) &&
		    !dc_intr()) {
			my_usleep(10000); /* FIXME */
		} else
			break;
	}
	errno = 0;
	/*if(ret<0) errno = ((InputStream *)inStream)->error; */

	return ret;
}

static int ogg_seek_cb(void *vdata, ogg_int64_t offset, int whence)
{
	const OggCallbackData *data = (const OggCallbackData *) vdata;
	if (dc_intr())
		return -1;
	return seekInputStream(data->inStream, offset, whence);
}

/* TODO: check Ogg libraries API and see if we can just not have this func */
static int ogg_close_cb(mpd_unused void *vdata)
{
	return 0;
}

static long ogg_tell_cb(void *vdata)
{
	const OggCallbackData *data = (const OggCallbackData *) vdata;

	return (long)(data->inStream->offset);
}

static const char *ogg_parseComment(const char *comment, const char *needle)
{
	int len = strlen(needle);

	if (strncasecmp(comment, needle, len) == 0 && *(comment + len) == '=') {
		return comment + len + 1;
	}

	return NULL;
}

static void ogg_getReplayGainInfo(char **comments, ReplayGainInfo ** infoPtr)
{
	const char *temp;
	int found = 0;

	if (*infoPtr)
		freeReplayGainInfo(*infoPtr);
	*infoPtr = newReplayGainInfo();

	while (*comments) {
		if ((temp =
		     ogg_parseComment(*comments, "replaygain_track_gain"))) {
			(*infoPtr)->trackGain = atof(temp);
			found = 1;
		} else if ((temp = ogg_parseComment(*comments,
						    "replaygain_album_gain"))) {
			(*infoPtr)->albumGain = atof(temp);
			found = 1;
		} else if ((temp = ogg_parseComment(*comments,
						    "replaygain_track_peak"))) {
			(*infoPtr)->trackPeak = atof(temp);
			found = 1;
		} else if ((temp = ogg_parseComment(*comments,
						    "replaygain_album_peak"))) {
			(*infoPtr)->albumPeak = atof(temp);
			found = 1;
		}

		comments++;
	}

	if (!found) {
		freeReplayGainInfo(*infoPtr);
		*infoPtr = NULL;
	}
}

static const char *VORBIS_COMMENT_TRACK_KEY = "tracknumber";
static const char *VORBIS_COMMENT_DISC_KEY = "discnumber";

static unsigned int ogg_parseCommentAddToTag(char *comment,
					     unsigned int itemType,
					     MpdTag ** tag)
{
	const char *needle;
	unsigned int len;
	switch (itemType) {
	case TAG_ITEM_TRACK:
		needle = VORBIS_COMMENT_TRACK_KEY;
		break;
	case TAG_ITEM_DISC:
		needle = VORBIS_COMMENT_DISC_KEY;
		break;
	default:
		needle = mpdTagItemKeys[itemType];
	}
	len = strlen(needle);

	if (strncasecmp(comment, needle, len) == 0 && *(comment + len) == '=') {
		if (!*tag)
			*tag = newMpdTag();

		addItemToMpdTag(*tag, itemType, comment + len + 1);

		return 1;
	}

	return 0;
}

static MpdTag *oggCommentsParse(char **comments)
{
	MpdTag *tag = NULL;

	while (*comments) {
		int j;
		for (j = TAG_NUM_OF_ITEM_TYPES; --j >= 0;) {
			if (ogg_parseCommentAddToTag(*comments, j, &tag))
				break;
		}
		comments++;
	}

	return tag;
}

static void putOggCommentsIntoOutputBuffer(char *streamName,
					   char **comments,
					   float time)
{
	MpdTag *tag;

	tag = oggCommentsParse(comments);
	if (!tag && streamName) {
		tag = newMpdTag();
	}
	if (!tag)
		return;

	if (streamName) {
		clearItemsFromMpdTag(tag, TAG_ITEM_NAME);
		addItemToMpdTag(tag, TAG_ITEM_NAME, streamName);
	}

	metadata_pipe_send(tag, time);
}

static float current_time(OggVorbis_File *vf)
{
	return (ov_pcm_tell(vf) / dc.audio_format.sampleRate);
}

/* public */
static int oggvorbis_decode(InputStream * inStream)
{
	OggVorbis_File vf;
	ov_callbacks callbacks;
	OggCallbackData data;
	int current_section;
	int prev_section = -1;
	long ret;
#define OGG_CHUNK_SIZE 4096
	char chunk[OGG_CHUNK_SIZE];
	int chunkpos = 0;
	long bitRate = 0;
	long test;
	ReplayGainInfo *replayGainInfo = NULL;
	char **comments;
	const char *errorStr;

	data.inStream = inStream;

	callbacks.read_func = ogg_read_cb;
	callbacks.seek_func = ogg_seek_cb;
	callbacks.close_func = ogg_close_cb;
	callbacks.tell_func = ogg_tell_cb;
	if ((ret = ov_open_callbacks(&data, &vf, NULL, 0, callbacks)) < 0) {
		if (!dc_intr()) {
			switch (ret) {
			case OV_EREAD:
				errorStr = "read error";
				break;
			case OV_ENOTVORBIS:
				errorStr = "not vorbis stream";
				break;
			case OV_EVERSION:
				errorStr = "vorbis version mismatch";
				break;
			case OV_EBADHEADER:
				errorStr = "invalid vorbis header";
				break;
			case OV_EFAULT:
				errorStr = "internal logic error";
				break;
			default:
				errorStr = "unknown error";
				break;
			}
			ERROR("Error decoding Ogg Vorbis stream: %s\n",
			      errorStr);
			return -1;
		}
		return 0;
	}
	dc.total_time = ov_time_total(&vf, -1);
	if (dc.total_time < 0)
		dc.total_time = 0;
	dc.audio_format.bits = 16;

	while (1) {
		if (dc_seek()) {
			dc_action_begin();
			if (0 == ov_time_seek_page(&vf, dc.seek_where))
				chunkpos = 0;
			else
				dc.seek_where = DC_SEEK_ERROR;
			dc_action_end();
		}
		ret = ov_read(&vf, chunk + chunkpos,
			      OGG_CHUNK_SIZE - chunkpos,
			      OGG_DECODE_USE_BIGENDIAN, 2, 1, &current_section);
		if (current_section != prev_section) {
			/*printf("new song!\n"); */
			vorbis_info *vi = ov_info(&vf, -1);
			dc.audio_format.channels = vi->channels;
			dc.audio_format.sampleRate = vi->rate;
			comments = ov_comment(&vf, -1)->user_comments;
			putOggCommentsIntoOutputBuffer(inStream->metaName,
						       comments,
						       current_time(&vf));
			ogg_getReplayGainInfo(comments, &replayGainInfo);
		}

		prev_section = current_section;

		if (ret <= 0) {
			if (ret == OV_HOLE) /* bad packet */
				ret = 0;
			else /* break on EOF or other error */
				break;
		}

		chunkpos += ret;

		if (chunkpos >= OGG_CHUNK_SIZE) {
			if ((test = ov_bitrate_instant(&vf)) > 0) {
				bitRate = test / 1000;
			}
			ob_send(chunk, chunkpos, current_time(&vf),
			        bitRate, replayGainInfo);
			chunkpos = 0;
			if (dc_intr())
				break;
		}
	}

	if (!dc_intr() && chunkpos > 0) {
		ob_send(chunk, chunkpos, ov_time_tell(&vf), bitRate,
		        replayGainInfo);
	}

	if (replayGainInfo)
		freeReplayGainInfo(replayGainInfo);

	ov_clear(&vf);

	return 0;
}

static MpdTag *oggvorbis_TagDup(char *file)
{
	MpdTag *ret;
	FILE *fp;
	OggVorbis_File vf;

	fp = fopen(file, "r");
	if (!fp) {
		DEBUG("oggvorbis_TagDup: Failed to open file: '%s', %s\n",
		      file, strerror(errno));
		return NULL;
	}
	if (ov_open(fp, &vf, NULL, 0) < 0) {
		fclose(fp);
		return NULL;
	}

	ret = oggCommentsParse(ov_comment(&vf, -1)->user_comments);

	if (!ret)
		ret = newMpdTag();
	ret->time = (int)(ov_time_total(&vf, -1) + 0.5);

	ov_clear(&vf);

	return ret;
}

static unsigned int oggvorbis_try_decode(InputStream * inStream)
{
	return (ogg_stream_type_detect(inStream) == VORBIS) ? 1 : 0;
}

static const char *oggvorbis_Suffixes[] = { "ogg","oga", NULL };
static const char *oggvorbis_MimeTypes[] = { "application/ogg",
					     "audio/x-vorbis+ogg",
					     "application/x-ogg",
					     NULL };

InputPlugin oggvorbisPlugin = {
	"oggvorbis",
	NULL,
	NULL,
	oggvorbis_try_decode,
	oggvorbis_decode,
	NULL,
	oggvorbis_TagDup,
	INPUT_PLUGIN_STREAM_URL | INPUT_PLUGIN_STREAM_FILE,
	oggvorbis_Suffixes,
	oggvorbis_MimeTypes
};

#else /* !HAVE_OGGVORBIS */

InputPlugin oggvorbisPlugin;

#endif /* HAVE_OGGVORBIS */
