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
#include "tagTracker.h"
#include "mpd_types.h"

#include <sys/stat.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>
#include <errno.h>
#ifdef HAVE_OGGVORBIS
#include <vorbis/vorbisfile.h>
#endif
#ifdef HAVE_FLAC
#include <FLAC/file_decoder.h>
#include <FLAC/metadata.h>
#endif

#ifdef HAVE_ID3TAG
#ifndef ID3_FRAME_COMPOSER
#define ID3_FRAME_COMPOSER "TCOM"
#endif
#endif

char * mpdTagItemKeys[TAG_NUM_OF_ITEM_TYPES] =
{
	"Artist",
	"Album",
	"Title",
	"Track",
	"Name",
	"Genre",
	"Date",
	"Composer",
	"Performer",
	"Comment"
};

static mpd_sint8 ignoreTagItems[TAG_NUM_OF_ITEM_TYPES];

void initTagConfig() {
	int quit = 0;
	char * temp;
	char * s;
	char * c;
	ConfigParam * param;
	int i;

	/* parse the "metadata_to_use" config parameter below */
	
	memset(ignoreTagItems, 0, TAG_NUM_OF_ITEM_TYPES);
	ignoreTagItems[TAG_ITEM_COMMENT] = 1; /* ignore comments by default */

	param = getConfigParam(CONF_METADATA_TO_USE);
	
	if(!param) return;

	memset(ignoreTagItems, 1, TAG_NUM_OF_ITEM_TYPES);

	if(0 == strcasecmp(param->value, "none")) return;

	temp = c = s = strdup(param->value);
	while(!quit) {
		if(*s == ',' || *s == '\0') {
			if(*s == '\0') quit = 1;
			*s = '\0';
			for(i = 0; i < TAG_NUM_OF_ITEM_TYPES; i++) {
				if(strcasecmp(c, mpdTagItemKeys[i]) == 0) {
					ignoreTagItems[i] = 0;
					break;
				}
			}
			if(strlen(c) && i == TAG_NUM_OF_ITEM_TYPES) {
				ERROR("error parsing metadata item \"%s\" at "
					"line %i\n", c, param->line);
				exit(EXIT_FAILURE);
			}
			s++;
			c = s;
		}
		s++;
	}

	free(temp);
}

void printMpdTag(FILE * fp, MpdTag * tag) {
	int i;

	if(tag->time>=0) myfprintf(fp,"Time: %i\n",tag->time);

	for(i = 0; i < tag->numOfItems; i++) {
		myfprintf(fp, "%s: %s\n", mpdTagItemKeys[tag->items[i].type],
				tag->items[i].value);
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
	if(!frame) return mpdTag;

	field = &frame->fields[1];
	nstrings = id3_field_getnstrings(field);

	for(i = 0; i < nstrings; i++) {
		ucs4 = id3_field_getstrings(field, i);
		assert(ucs4);

		if(type == TAG_ITEM_GENRE) {
			ucs4 = id3_genre_name(ucs4);
		}

		utf8 = id3_ucs4_utf8duplicate(ucs4);

		if(!utf8) continue;

		if( NULL == mpdTag ) mpdTag = newMpdTag();
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
	ret = getID3Info(tag, ID3_FRAME_COMPOSER, TAG_ITEM_COMPOSER, ret);
	ret = getID3Info(tag, ID3_FRAME_COMMENT, TAG_ITEM_COMMENT, ret);

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
		DEBUG("id3Dup: Failed to open file: '%s', %s\n",file, strerror(errno));
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

MpdTag * apeDup(char * file) {
	MpdTag * ret = NULL;
	FILE * fp = NULL;
	int tagCount;
	unsigned char * buffer = NULL;
	unsigned char * p;
	int tagLen;
	int size;
	unsigned long flags;
	int i;
	unsigned char * key;

	struct {
		unsigned char id[8];
		unsigned char version[4];
		unsigned char length[4];
		unsigned char tagCount[4];
		unsigned char flags[4];
		unsigned char reserved[8];
	} footer;

	char * apeItems[7] =
	{
		"title",
		"artist",
		"album",
		"comment",
		"genre",
		"track",
		"year"
	};

	int tagItems[7] =
	{
		TAG_ITEM_TITLE,
		TAG_ITEM_ARTIST,
		TAG_ITEM_ALBUM,
		TAG_ITEM_COMMENT,
		TAG_ITEM_GENRE,
		TAG_ITEM_TRACK,
		TAG_ITEM_DATE,
	};

	fp = fopen(file, "r");
	if(!fp) return NULL;

	/* determine if file has an apeV2 tag */
	if(fseek(fp, 0, SEEK_END)) goto fail;
	size = ftell(fp);
	if(fseek(fp, size-sizeof(footer), SEEK_SET)) goto fail;
	if(fread(&footer, 1, sizeof(footer), fp) != sizeof(footer)) goto fail;
	if(memcmp(footer.id, "APETAGEX", sizeof(footer.id)) != 0) goto fail;
	if(readLEuint32(footer.version) != 2000) goto fail;

	/* find begining of ape tag */
	tagLen = readLEuint32(footer.length);
	if(tagLen < sizeof(footer)) goto fail;
	if(fseek(fp, size-tagLen, SEEK_SET)) goto fail;

	/* read tag into buffer */
	tagLen -= sizeof(footer);
	buffer = malloc(tagLen);
	if(fread(buffer, 1, tagLen, fp) != tagLen) goto fail;

	/* read tags */
	tagCount = readLEuint32(footer.tagCount);
	p = buffer;
	while(tagCount-- && tagLen > 10) {
		size = readLEuint32(p);
		p += 4;
		tagLen -= 4;
		flags = readLEuint32(p);
		p += 4;
		tagLen -= 4;

		/* get the key */
		key = p;
		while(tagLen-size > 0 && *p != '\0') {
			p++;
			tagLen--;
		}
		p++;
		tagLen--;

		/* get the value */
		if(tagLen-size < 0) goto fail;

		/* we only care about utf-8 text tags */
		if(!(flags & (0x3 << 1))) {
			for(i = 0; i < 7; i++) {
				if(strcasecmp(key, apeItems[i]) == 0) {
					if(!ret) ret = newMpdTag();
					addItemToMpdTagWithLen(
						ret, tagItems[i], p, size);
				}
			}
		}
		p += size;
		tagLen -= size;
	}
	
fail:
	if(fp) fclose(fp);
	if(buffer) free(buffer);
	return ret;
}

MpdTag * newMpdTag() {
	MpdTag * ret = malloc(sizeof(MpdTag));
	ret->items = NULL;
	ret->time = -1;
	ret->numOfItems = 0;
	return ret;
}

static void deleteItem(MpdTag * tag, int index) {
	tag->numOfItems--;

	assert(index < tag->numOfItems);

	removeTagItemString(tag->items[index].type, tag->items[index].value);
	/* free(tag->items[index].value); */

	if(tag->numOfItems-index > 0) {
		memmove(tag->items+index, tag->items+index+1, 
				tag->numOfItems-index);
	}

	if(tag->numOfItems > 0) {
		tag->items = realloc(tag->items, 
				tag->numOfItems*sizeof(MpdTagItem));
	}
	else {
		free(tag->items);
		tag->items = NULL;
	}
}

void clearItemsFromMpdTag(MpdTag * tag, int type) {
	int i = 0;

	for(i = 0; i < tag->numOfItems; i++) {
		if(tag->items[i].type == type) {
			deleteItem(tag, i);
			/* decrement since when just deleted this node */
			i--;
		}
	}
}

void clearMpdTag(MpdTag * tag) {
	int i;

	for(i = 0; i < tag->numOfItems; i++) {
		removeTagItemString(tag->items[i].type, tag->items[i].value);
		/* free(tag->items[i].value); */
	}

	if(tag->items) free(tag->items);
	tag->items = NULL;

	tag->numOfItems = 0;

	tag->time = -1;
}

void freeMpdTag(MpdTag * tag) {
        clearMpdTag(tag);
	free(tag);
}

MpdTag * mpdTagDup(MpdTag * tag) {
	MpdTag * ret = NULL;
	int i;

	if(!tag)  return NULL;

	ret = newMpdTag();
	ret->time = tag->time;

	for(i = 0; i < tag->numOfItems; i++) {
		addItemToMpdTag(ret, tag->items[i].type, tag->items[i].value);
	}

	return ret;
}

int mpdTagsAreEqual(MpdTag * tag1, MpdTag * tag2) {
	int i;

        if(tag1 == NULL && tag2 == NULL) return 1;
        else if(!tag1 || !tag2) return 0;

        if(tag1->time != tag2->time) return 0;

	if(tag1->numOfItems != tag2->numOfItems) return 0;

	for(i = 0; i < tag1->numOfItems; i++) {
		if(tag1->items[i].type != tag2->items[i].type) return 0;
		if(strcmp(tag1->items[i].value, tag2->items[i].value)) {
			return 0;
		}
	}

        return 1;
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

inline static void appendToTagItems(MpdTag * tag, int type, char * value, 
			int len) 
{
	int i = tag->numOfItems;
	
	char * dup;
	dup = malloc(len+1);
	strncpy(dup, value, len);
	dup[len] = '\0';

	fixUtf8(dup);
	stripReturnChar(dup);

	tag->numOfItems++;
	tag->items = realloc(tag->items, tag->numOfItems*sizeof(MpdTagItem));

	tag->items[i].type = type;
	tag->items[i].value = getTagItemString(type, dup);

	free(dup);
}

void addItemToMpdTagWithLen(MpdTag * tag, int itemType, char * value, int len) {
	if(ignoreTagItems[itemType]) return;

	if(!value || !len) return;

	/* we can't hold more than 255 items */
	if(tag->numOfItems == 255) return;
			
	appendToTagItems(tag, itemType, value, len);
}

char * getNextItemFromMpdTag(MpdTag * tag, int itemType, int * last) {
	int i = 0;

	if(last && *last >=0) i = *last+1;

	for(i = 0; i < tag->numOfItems; i++) {
		if(itemType == tag->items[i].type) {
			if(last) *last = i;
			return tag->items[i].value;
		}
		i++;
	}

	return NULL;
}
