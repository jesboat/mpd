/* the Music Player Daemon (MPD)
 * (c)2003-2004 by Warren Dukes (shank@mercury.chem.pitt.edu
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

#include "tag.h"
#include "path.h"
#include "myfprintf.h"
#include "utils.h"
#include "utf8.h"
#include "log.h"
#include "inputStream.h"
#include "conf.h"
#include "charConv.h"

#include <sys/stat.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>
#include <errno.h>
#ifdef HAVE_OGG
#include <vorbis/vorbisfile.h>
#endif
#ifdef HAVE_FLAC
#include <FLAC/file_decoder.h>
#include <FLAC/metadata.h>
#endif

char * mpdTagItemsKeys[TAG_NUM_OF_ITEM_TYPES] =
{
	"Artist",
	"Album",
	"Title",
	"Track",
	"Name",
	"Genre",
	"Date"
};

void printMpdTag(FILE * fp, MpdTag * tag) {
	MpdTagItem * item;

	for(item = tag->tagItems; item && item->type!=TAG_ITEM_END; item++) {
		myfprintf(fp, "%s: %s\n", mpdTagItemKeys[item->type],
				item->value);
	}

	if(tag->time>=0) myfprintf(fp,"Time: %i\n",tag->time);
}

#define fixUtf8(str) { \
	if(str && !validUtf8String(str)) { \
		char * temp; \
		DEBUG("not valid utf8 in tag: %s\n",str); \
		temp = latin1StrToUtf8Dup(str); \
		free(str); \
		str = temp; \
	} \
}

void validateUtf8Tag(MpdTag * tag) {
	MpdTagItem * item = tag->tagItems;

	while(item && item->type != TAG_ITEM_END) {
		fixUtf8(item->value);
		stripReturnChar(item->value);
		item++;
	}
}

#ifdef HAVE_ID3TAG
MpdTag * getID3Info(struct id3_tag * tag, char * id, int type, MpdTag * mpdTag) 
{
	struct id3_frame const * frame;
	id3_ucs4_t const * ucs4;
	id3_utf8_t * utf8;
	union id3_field const * field;
	unsigned int nstrings;
	int i;

	frame = id3_tag_findframe(tag, id, 0);
	if(!frame) return NULL;

	field = &frame->fields[1];
	nstrings = id3_field_getnstrings(field);
	if(nstrings<1) return NULL;

	for(i = 0; i < nstrings; i++) {
		ucs4 = id3_field_getstrings(field,0);
		assert(ucs4);

		if(type == TAG_ITEM_GENRE) {
			ucs4 = id3_genre_name(ucs4);
		}

		utf8 = id3_ucs4_utf8duplicate(ucs4);
		if(!utf8) continue;

		if( NULL == mpdTag ) mpdTag == newMpdTag();
		addItemToMpdTag(mpdTag, type, utf8);

		free(utf8);
	}

	return mpdTag;
}
#endif

#ifdef HAVE_ID3TAG
MpdTag * parseId3Tag(struct id3_tag * tag) {
	MpdTag * ret = NULL;

	ret = getID3Info(tag, ID3_FRAME_ARTIST, TAG_ITEM_ARTIST, ret);
	ret = getID3Info(tag, ID3_FRAME_TITLE, TAG_ITEM_TITLE, ret);
	ret = getID3Info(tag, ID3_FRAME_ALBUM, TAG_ITEM_ALBUM, ret);
	ret = getID3Info(tag, ID3_FRAME_TRACK, TAG_ITEM_TRACK, ret);
	ret = getID3Info(tag, ID3_FRAME_YEAR, TAG_ITEM_DATE, ret);
	ret = getID3Info(tag, ID3_FRAME_GENRE, TAG_ITEM_GENRE, ret);

	return ret;
}
#endif

MpdTag * id3Dup(char * file) {
	MpdTag * ret = NULL;
#ifdef HAVE_ID3TAG
	struct id3_file * id3_file;
	struct id3_tag * tag;

	id3_file = id3_file_open(file, ID3_FILE_MODE_READONLY);
			
	if(!id3_file) {
		return NULL;
	}

	tag = id3_file_tag(id3_file);
	if(!tag) {
		id3_file_close(id3_file);
		return NULL;
	}

	ret = parseId3Tag(tag);

	id3_file_close(id3_file);

#endif
	return ret;	
}

MpdTag * newMpdTag() {
	MpdTag * ret = malloc(sizeof(MpdTag));
	ret->tagItems = NULL;
	ret->time = -1;
	return ret;
}

void clearMpdTag(MpdTag * tag) {
	MpdTagItem * item;

	for(item = tag->tagItems; item && item->type != TAG_ITEM_END; item++) {
		free(item->value);
	}

	if(tag->tagItems) free(tag->tagItems);
	tag->tagItems = NULL;
}

void freeMpdTag(MpdTag * tag) {
        clearMpdTag(tag);
	free(tag);
}

MpdTag * mpdTagDup(MpdTag * tag) {
	MpdTag * ret = NULL;
	MpdTagItem * item;

	if(!tag)  return NULL;

	ret = newMpdTag();
	ret->time = tag->time;

	for(item = tag->tagItems; item && item->type != TAG_ITEM_END; item++) {
		addItemToMpdTag(ret, item->type, item->value);
	}

	return ret;
}

int mpdTagsAreEqual(MpdTag * tag1, MpdTag * tag2) {
	MpdTagItem * item1;
	MpdTagItem * item2;

        if(tag1 == NULL && tag2 == NULL) return 1;
        else if(!tag1 || !tag2) return 0;

        if(tag1->time != tag2->time) return 0;

	for(item1 = tag1->tagItems, item2 = tag2->tagItems; 
			item1 && item1->type != TAG_ITEM_END;
			item1++, item2++)
	{
		if(!item2) return 0;
		if(item1->type != item2->type) return 0;
		if(0 == strcmp(item1->value, item2->value)) return 0;
	}

	if(item2 && !item1) return 0;
	if(item2->type != item1->type) return 0;

        return 1;
}
