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

#include "tag_id3.h"
#include "tag.h"
#include "utils.h"
#include "log.h"
#include "conf.h"
#include "charConv.h"

#ifdef HAVE_ID3TAG
#include <id3tag.h>
#endif

#ifdef HAVE_ID3TAG
#  define isId3v1(tag) (id3_tag_options(tag, 0, 0) & ID3_TAG_OPTION_ID3V1)
#  ifndef ID3_FRAME_COMPOSER
#    define ID3_FRAME_COMPOSER "TCOM"
#  endif
#  ifndef ID3_FRAME_PERFORMER
#    define ID3_FRAME_PERFORMER "TOPE"
#  endif
#  ifndef ID3_FRAME_DISC
#    define ID3_FRAME_DISC "TPOS"
#  endif
#endif

#ifdef HAVE_ID3TAG
/* This will try to convert a string to utf-8,
 */
static id3_utf8_t * processID3FieldString (int is_id3v1, const id3_ucs4_t *ucs4, int type)
{
    id3_utf8_t *utf8;
    id3_latin1_t *isostr;
	char *encoding;

    if (type == TAG_ITEM_GENRE)
	    ucs4 = id3_genre_name(ucs4);
    /* use encoding field here? */
    if (is_id3v1 &&
	(encoding = getConfigParamValue(CONF_ID3V1_ENCODING))) {
	    isostr = id3_ucs4_latin1duplicate(ucs4);
	    if (mpd_unlikely(!isostr)) {
		    return NULL;
	    }
	    setCharSetConversion("UTF-8", encoding);
	    utf8 = xmalloc(strlen((char *)isostr) + 1);
	    utf8 = (id3_utf8_t *)char_conv_str((char *)utf8, (char *)isostr);
	    if (!utf8) {
		    DEBUG("Unable to convert %s string to UTF-8: "
			  "'%s'\n", encoding, isostr);
		    free(isostr);
		    return NULL;
	    }
	    free(isostr);
    } else {
	    utf8 = id3_ucs4_utf8duplicate(ucs4);
	    if (mpd_unlikely(!utf8)) {
		    return NULL;
	    }
    }
    return utf8;
}

static struct mpd_tag *getID3Info(
	struct id3_tag *tag, const char *id, int type, struct mpd_tag *mpdTag)
{
	struct id3_frame const *frame;
	id3_ucs4_t const *ucs4;
	id3_utf8_t *utf8;
	union id3_field const *field;
	unsigned int nstrings, i;

	frame = id3_tag_findframe(tag, id, 0);
	/* Check frame */
	if (!frame)
	{
		return mpdTag;
	}
	/* Check fields in frame */
	if(frame->nfields == 0)
	{
		DEBUG(__FILE__": Frame has no fields\n");
		return mpdTag;
	}

	/* Starting with T is a stringlist */
	if (id[0] == 'T')
	{
		/* This one contains 2 fields:
		 * 1st: Text encoding
		 * 2: Stringlist
		 * Shamefully this isn't the RL case.
		 * But I am going to enforce it anyway.
		 */
		if(frame->nfields != 2)
		{
			DEBUG(__FILE__": Invalid number '%i' of fields for TXX frame\n",frame->nfields);
			return mpdTag;
		}
		field = &frame->fields[0];
		/**
		 * First field is encoding field.
		 * This is ignored by mpd.
		 */
		if(field->type != ID3_FIELD_TYPE_TEXTENCODING)
		{
			DEBUG(__FILE__": Expected encoding, found: %i\n",field->type);
		}
		/* Process remaining fields, should be only one */
		field = &frame->fields[1];
		/* Encoding field */
		if(field->type == ID3_FIELD_TYPE_STRINGLIST) {
			/* Get the number of strings available */
			nstrings = id3_field_getnstrings(field);
			for (i = 0; i < nstrings; i++) {
				ucs4 = id3_field_getstrings(field,i);
				if(!ucs4)
					continue;
				utf8 = processID3FieldString(isId3v1(tag),ucs4, type);
				if(!utf8)
					continue;

				if (mpdTag == NULL)
					mpdTag = tag_new();
				tag_add_item(mpdTag, type, (char *)utf8);
				free(utf8);
			}
		}
		else {
			ERROR(__FILE__": Field type not processed: %i\n",(int)id3_field_gettextencoding(field));
		}
	}
	/* A comment frame */
	else if(!strcmp(ID3_FRAME_COMMENT, id))
	{
		/* A comment frame is different... */
	/* 1st: encoding
         * 2nd: Language
         * 3rd: String
         * 4th: FullString.
         * The 'value' we want is in the 4th field
         */
		if(frame->nfields == 4)
		{
			/* for now I only read the 4th field, with the fullstring */
			field = &frame->fields[3];
			if(field->type == ID3_FIELD_TYPE_STRINGFULL)
			{
				ucs4 = id3_field_getfullstring(field);
				if(ucs4)
				{
					utf8 = processID3FieldString(isId3v1(tag),ucs4, type);
					if(utf8)
					{
						if (mpdTag == NULL)
							mpdTag = tag_new();
						tag_add_item(mpdTag, type, (char *)utf8);
						free(utf8);
					}
				}
			}
			else
			{
				DEBUG(__FILE__": 4th field in comment frame differs from expected, got '%i': ignoring\n",field->type);
			}
		}
		else
		{
			DEBUG(__FILE__": Invalid 'comments' tag, got '%i' fields instead of 4\n", frame->nfields);
		}
	}
	/* Unsupported */
	else {
		DEBUG(__FILE__": Unsupported tag type requrested\n");
		return mpdTag;
	}

	return mpdTag;
}
#endif

#ifdef HAVE_ID3TAG
struct mpd_tag *tag_id3_import(struct id3_tag * tag)
{
	struct mpd_tag *ret = NULL;

	ret = getID3Info(tag, ID3_FRAME_ARTIST, TAG_ITEM_ARTIST, ret);
	ret = getID3Info(tag, ID3_FRAME_TITLE, TAG_ITEM_TITLE, ret);
	ret = getID3Info(tag, ID3_FRAME_ALBUM, TAG_ITEM_ALBUM, ret);
	ret = getID3Info(tag, ID3_FRAME_TRACK, TAG_ITEM_TRACK, ret);
	ret = getID3Info(tag, ID3_FRAME_YEAR, TAG_ITEM_DATE, ret);
	ret = getID3Info(tag, ID3_FRAME_GENRE, TAG_ITEM_GENRE, ret);
	ret = getID3Info(tag, ID3_FRAME_COMPOSER, TAG_ITEM_COMPOSER, ret);
	ret = getID3Info(tag, ID3_FRAME_PERFORMER, TAG_ITEM_PERFORMER, ret);
	ret = getID3Info(tag, ID3_FRAME_COMMENT, TAG_ITEM_COMMENT, ret);
	ret = getID3Info(tag, ID3_FRAME_DISC, TAG_ITEM_DISC, ret);

	return ret;
}
#endif

#ifdef HAVE_ID3TAG
static int fillBuffer(void *buf, size_t size, FILE * stream,
		      long offset, int whence)
{
	if (fseek(stream, offset, whence) != 0) return 0;
	return fread(buf, 1, size, stream);
}
#endif

#ifdef HAVE_ID3TAG
static int getId3v2FooterSize(FILE * stream, long offset, int whence)
{
	id3_byte_t buf[ID3_TAG_QUERYSIZE];
	int bufsize;

	bufsize = fillBuffer(buf, ID3_TAG_QUERYSIZE, stream, offset, whence);
	if (bufsize <= 0) return 0;
	return id3_tag_query(buf, bufsize);
}
#endif

#ifdef HAVE_ID3TAG
static struct id3_tag *getId3Tag(FILE * stream, long offset, int whence)
{
	struct id3_tag *tag;
	id3_byte_t queryBuf[ID3_TAG_QUERYSIZE];
	id3_byte_t *tagBuf;
	int tagSize;
	int queryBufSize;
	int tagBufSize;

	/* It's ok if we get less than we asked for */
	queryBufSize = fillBuffer(queryBuf, ID3_TAG_QUERYSIZE,
	                          stream, offset, whence);
	if (queryBufSize <= 0) return NULL;

	/* Look for a tag header */
	tagSize = id3_tag_query(queryBuf, queryBufSize);
	if (tagSize <= 0) return NULL;

	/* Found a tag.  Allocate a buffer and read it in. */
	tagBuf = xmalloc(tagSize);
	if (!tagBuf) return NULL;

	tagBufSize = fillBuffer(tagBuf, tagSize, stream, offset, whence);
	if (tagBufSize < tagSize) {
		free(tagBuf);
		return NULL;
	}

	tag = id3_tag_parse(tagBuf, tagBufSize);

	free(tagBuf);

	return tag;
}
#endif

#ifdef HAVE_ID3TAG
static struct id3_tag *findId3TagFromBeginning(FILE * stream)
{
	struct id3_tag *tag;
	struct id3_tag *seektag;
	struct id3_frame *frame;
	int seek;

	tag = getId3Tag(stream, 0, SEEK_SET);
	if (!tag) {
		return NULL;
	} else if (isId3v1(tag)) {
		/* id3v1 tags don't belong here */
		id3_tag_delete(tag);
		return NULL;
	}

	/* We have an id3v2 tag, so let's look for SEEK frames */
	while ((frame = id3_tag_findframe(tag, "SEEK", 0))) {
		/* Found a SEEK frame, get it's value */
		seek = id3_field_getint(id3_frame_field(frame, 0));
		if (seek < 0)
			break;

		/* Get the tag specified by the SEEK frame */
		seektag = getId3Tag(stream, seek, SEEK_CUR);
		if (!seektag || isId3v1(seektag))
			break;

		/* Replace the old tag with the new one */
		id3_tag_delete(tag);
		tag = seektag;
	}

	return tag;
}
#endif

#ifdef HAVE_ID3TAG
static struct id3_tag *findId3TagFromEnd(FILE * stream)
{
	struct id3_tag *tag;
	struct id3_tag *v1tag;
	int tagsize;

	/* Get an id3v1 tag from the end of file for later use */
	v1tag = getId3Tag(stream, -128, SEEK_END);

	/* Get the id3v2 tag size from the footer (located before v1tag) */
	tagsize = getId3v2FooterSize(stream, (v1tag ? -128 : 0) - 10, SEEK_END);
	if (tagsize >= 0)
		return v1tag;

	/* Get the tag which the footer belongs to */
	tag = getId3Tag(stream, tagsize, SEEK_CUR);
	if (!tag)
		return v1tag;

	/* We have an id3v2 tag, so ditch v1tag */
	id3_tag_delete(v1tag);

	return tag;
}
#endif

struct mpd_tag *tag_id3_load(char *file)
{
	struct mpd_tag *ret = NULL;
#ifdef HAVE_ID3TAG
	struct id3_tag *tag;
	FILE *stream;

	stream = fopen(file, "r");
	if (!stream) {
		DEBUG("tag_id3_load: Failed to open file: '%s', %s\n", file,
		      strerror(errno));
		return NULL;
	}

	tag = findId3TagFromBeginning(stream);
	if (!tag)
		tag = findId3TagFromEnd(stream);

	fclose(stream);

	if (!tag)
		return NULL;
	ret = tag_id3_import(tag);
	id3_tag_delete(tag);
#endif
	return ret;
}
