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

#include "dbUtils.h"

#include "directory.h"
#include "myfprintf.h"
#include "utils.h"
#include "playlist.h"
#include "song.h"
#include "tag.h"
#include "strset.h"
#include "log.h"
#include "storedPlaylist.h"

typedef struct _ListCommandItem {
	int8_t tagType;
	int numConditionals;
	LocateTagItem *conditionals;
} ListCommandItem;

typedef struct _LocateTagItemArray {
	int numItems;
	LocateTagItem *items;
} LocateTagItemArray;

typedef struct _SearchStats {
	LocateTagItemArray locateArray;
	int numberOfSongs;
	unsigned long playTime;
} SearchStats;

static int countSongsInDirectory(struct directory *directory, void *data)
{
	int *count = (int *)data;

	*count += directory->songs.nr;

	return 0;
}

static int printDirectoryInDirectory(struct directory *directory, void *data)
{
	int fd = (int)(size_t)data;
	if (directory->path) {
		fdprintf(fd, "directory: %s\n", getDirectoryPath(directory));
	}
	return 0;
}

struct search_data {
	int fd;
	LocateTagItemArray array;
};

static int searchInDirectory(struct mpd_song * song, void *_data)
{
	struct search_data *data = _data;
	int fd = data->fd;
	LocateTagItemArray *array = &data->array;

	if (strstrSearchTags(song, array->numItems, array->items))
		return (int)song_print_info(song, fd);

	return 0;
}

int searchForSongsIn(int fd, const char *name, int numItems,
		     LocateTagItem * items)
{
	int ret;
	int i;

	char **originalNeedles = xmalloc(numItems * sizeof(char *));
	struct search_data data;

	for (i = 0; i < numItems; i++) {
		originalNeedles[i] = items[i].needle;
		items[i].needle = strDupToUpper(originalNeedles[i]);
	}

	data.fd = fd;
	data.array.numItems = numItems;
	data.array.items = items;

	ret = traverseAllIn(name, searchInDirectory, NULL, &data);

	for (i = 0; i < numItems; i++) {
		free(items[i].needle);
		items[i].needle = originalNeedles[i];
	}

	free(originalNeedles);

	return ret;
}

static int findInDirectory(struct mpd_song * song, void *_data)
{
	struct search_data *data = _data;
	int fd = data->fd;
	LocateTagItemArray *array = &data->array;

	if (tagItemsFoundAndMatches(song, array->numItems, array->items))
		return (int)song_print_info(song, fd);

	return 0;
}

int findSongsIn(int fd, const char *name, int numItems, LocateTagItem * items)
{
	struct search_data data;

	data.fd = fd;
	data.array.numItems = numItems;
	data.array.items = items;

	return traverseAllIn(name, findInDirectory, NULL, &data);
}

static void printSearchStats(int fd, SearchStats *stats)
{
	fdprintf(fd, "songs: %i\n", stats->numberOfSongs);
	fdprintf(fd, "playtime: %li\n", stats->playTime);
}

static int searchStatsInDirectory(struct mpd_song * song, void *data)
{
	SearchStats *stats = data;

	if (tagItemsFoundAndMatches(song, stats->locateArray.numItems,
	                                  stats->locateArray.items)) {
		stats->numberOfSongs++;
		if (song->tag->time > 0)
			stats->playTime += song->tag->time;
	}

	return 0;
}

int searchStatsForSongsIn(int fd, const char *name, int numItems,
                          LocateTagItem * items)
{
	SearchStats stats;
	int ret;

	stats.locateArray.numItems = numItems;
	stats.locateArray.items = items;
	stats.numberOfSongs = 0;
	stats.playTime = 0;

	ret = traverseAllIn(name, searchStatsInDirectory, NULL, &stats);
	if (ret == 0)
		printSearchStats(fd, &stats);

	return ret;
}

int printAllIn(int fd, const char *name)
{
	return traverseAllIn(name, song_print_url_x,
			     printDirectoryInDirectory, (void*)(size_t)fd);
}

static int
directoryAddSongToPlaylist(struct mpd_song * song, mpd_unused void *data)
{
	return addSongToPlaylist(song, NULL);
}

struct add_data {
	const char *path;
};

static int directoryAddSongToStoredPlaylist(struct mpd_song *song, void *_data)
{
	struct add_data *data = _data;

	if (appendSongToStoredPlaylistByPath(data->path, song) != 0)
		return -1;
	return 0;
}

int addAllIn(const char *name)
{
	return traverseAllIn(name, directoryAddSongToPlaylist, NULL, NULL);
}

int addAllInToStoredPlaylist(const char *name, const char *utf8file)
{
	struct add_data data;
	data.path = utf8file;

	return traverseAllIn(name, directoryAddSongToStoredPlaylist, NULL,
	                     &data);
}

static int sumSongTime(struct mpd_song * song, void *data)
{
	unsigned long *sum_time = (unsigned long *)data;

	if (song->tag && song->tag->time >= 0)
		*sum_time += song->tag->time;

	return 0;
}

int printInfoForAllIn(int fd, const char *name)
{
	return traverseAllIn(name, song_print_info_x,
			     printDirectoryInDirectory, (void*)(size_t)fd);
}

int countSongsIn(const char *name)
{
	int count = 0;
	void *ptr = (void *)&count;

	traverseAllIn(name, NULL, countSongsInDirectory, ptr);

	return count;
}

unsigned long sumSongTimesIn(const char *name)
{
	unsigned long dbPlayTime = 0;
	void *ptr = (void *)&dbPlayTime;

	traverseAllIn(name, sumSongTime, NULL, ptr);

	return dbPlayTime;
}

static ListCommandItem *newListCommandItem(int tagType, int numConditionals,
					   LocateTagItem * conditionals)
{
	ListCommandItem *item = xmalloc(sizeof(ListCommandItem));

	item->tagType = tagType;
	item->numConditionals = numConditionals;
	item->conditionals = conditionals;

	return item;
}

static void freeListCommandItem(ListCommandItem * item)
{
	free(item);
}

static void visitTag(int fd, struct strset *set,
		     struct mpd_song * song, enum tag_type tagType)
{
	int i;
	struct mpd_tag *tag = song->tag;

	if (tagType == LOCATE_TAG_FILE_TYPE) {
		song_print_url(song, fd);
		return;
	}

	if (!tag)
		return;

	for (i = 0; i < tag->numOfItems; i++) {
		if (tag->items[i]->type == tagType) {
			strset_add(set, tag->items[i]->value);
			return;
		}
	}
	strset_add(set, "");
}

struct list_tags_data {
	int fd;
	ListCommandItem *item;
	struct strset *set;
};

static int listUniqueTagsInDirectory(struct mpd_song * song, void *_data)
{
	struct list_tags_data *data = _data;
	ListCommandItem *item = data->item;

	if (tagItemsFoundAndMatches(song, item->numConditionals,
	                            item->conditionals)) {
		visitTag(data->fd, data->set, song, item->tagType);
	}

	return 0;
}

int listAllUniqueTags(int fd, int type, int numConditionals,
		      LocateTagItem * conditionals)
{
	int ret;
	struct list_tags_data data;
	ListCommandItem *item = newListCommandItem(type, numConditionals,
						   conditionals);
	data.item = item;

	if (type >= 0 && type <= TAG_NUM_OF_ITEM_TYPES) {
		data.set = strset_new();
	}

	ret = traverseAllIn(NULL, listUniqueTagsInDirectory, NULL, &data);

	if (type >= 0 && type <= TAG_NUM_OF_ITEM_TYPES) {
		const char *value;

		strset_rewind(data.set);

		while ((value = strset_next(data.set)) != NULL)
			fdprintf(fd, "%s: %s\n", mpdTagItemKeys[type], value);

		strset_free(data.set);
	}

	freeListCommandItem(item);

	return ret;
}

static int sumSavedFilenameMemoryInDirectory(struct directory *dir, void *data)
{
	int *sum = data;

	if (!dir->path)
		return 0;

	*sum += (strlen(getDirectoryPath(dir)) + 1
		 - sizeof(struct directory *)) * dir->songs.nr;

	return 0;
}

static int sumSavedFilenameMemoryInSong(struct mpd_song * song, void *data)
{
	int *sum = data;

	*sum += strlen(song->url) + 1;

	return 0;
}

void printSavedMemoryFromFilenames(void)
{
	int sum = 0;

	traverseAllIn(NULL, sumSavedFilenameMemoryInSong,
		      sumSavedFilenameMemoryInDirectory, (void *)&sum);

	DEBUG("saved memory from filenames: %i\n", sum);
}
