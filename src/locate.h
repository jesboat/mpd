/* the Music Player Daemon (MPD)
 * (c)2007 by Warren Dukes (warren.dukes@gmail.com)
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

#ifndef LOCATE_H
#define LOCATE_H

#include "song.h"

#define LOCATE_TAG_FILE_TYPE	TAG_NUM_OF_ITEM_TYPES+10
#define LOCATE_TAG_ANY_TYPE     TAG_NUM_OF_ITEM_TYPES+20

/* struct used for search, find, list queries */
typedef struct _LocateTagItem {
	int8_t tagType;
	/* what we are looking for */
	char *needle;
} LocateTagItem;

int getLocateTagItemType(const char *str);

/* returns NULL if not a known type */
LocateTagItem *newLocateTagItem(const char *typeString, const char *needle);

/* return number of items or -1 on error */
int newLocateTagItemArrayFromArgArray(char *argArray[], int numArgs,
				      LocateTagItem ** arrayRet);

void freeLocateTagItemArray(int count, LocateTagItem * array);

void freeLocateTagItem(LocateTagItem * item);

int strstrSearchTags(Song * song, int numItems, LocateTagItem * items);

int tagItemsFoundAndMatches(Song * song, int numItems, LocateTagItem * items);

#endif
