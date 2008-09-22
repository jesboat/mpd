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

#include "song.h"
#include "ls.h"
#include "directory.h"
#include "utils.h"
#include "tag.h"
#include "log.h"
#include "path.h"
#include "playlist.h"
#include "inputPlugin.h"
#include "myfprintf.h"

#include "os_compat.h"

static Song *newNullSong(void)
{
	Song *song = xmalloc(sizeof(Song));

	song->tag = NULL;
	song->url = NULL;
	song->type = SONG_TYPE_FILE;
	song->parentDir = NULL;

	return song;
}

Song *newSong(const char *url, int type, Directory * parentDir)
{
	Song *song;

	if (strchr(url, '\n')) {
		DEBUG("newSong: '%s' is not a valid uri\n", url);
		return NULL;
	}

	song = newNullSong();

	song->url = xstrdup(url);
	song->type = type;
	song->parentDir = parentDir;

	assert(type == SONG_TYPE_URL || parentDir);

	if (song->type == SONG_TYPE_FILE) {
		InputPlugin *plugin;
		unsigned int next = 0;
		char path_max_tmp[MPD_PATH_MAX];
		char *abs_path = rmp2amp_r(path_max_tmp,
		                           get_song_url(path_max_tmp, song));

		while (!song->tag && (plugin = isMusic(abs_path,
						       &(song->mtime),
						       next++))) {
			song->tag = plugin->tagDupFunc(abs_path);
		}
		if (!song->tag || song->tag->time < 0) {
			freeSong(song);
			song = NULL;
		}
	}

	return song;
}

void freeSong(Song * song)
{
	deleteASongFromPlaylist(song);
	freeJustSong(song);
}

void freeJustSong(Song * song)
{
	free(song->url);
	if (song->tag)
		tag_free(song->tag);
	free(song);
}

SongList *newSongList(void)
{
	return makeList((ListFreeDataFunc *) freeSong, 0);
}

Song *addSongToList(SongList * list, const char *url, const char *utf8path,
		    int songType, Directory * parentDirectory)
{
	Song *song = NULL;

	switch (songType) {
	case SONG_TYPE_FILE:
		if (isMusic(utf8path, NULL, 0)) {
			song = newSong(url, songType, parentDirectory);
		}
		break;
	case SONG_TYPE_URL:
		song = newSong(url, songType, parentDirectory);
		break;
	default:
		DEBUG("addSongToList: Trying to add an invalid song type\n");
	}

	if (song == NULL)
		return NULL;

	insertInList(list, song->url, (void *)song);

	return song;
}

void freeSongList(SongList * list)
{
	freeList(list);
}

void printSongUrl(int fd, Song * song)
{
	if (song->parentDir && song->parentDir->path) {
		fdprintf(fd, "%s%s/%s\n", SONG_FILE,
			  getDirectoryPath(song->parentDir), song->url);
	} else {
		fdprintf(fd, "%s%s\n", SONG_FILE, song->url);
	}
}

int printSongInfo(int fd, Song * song)
{
	printSongUrl(fd, song);

	if (song->tag)
		tag_print(fd, song->tag);

	return 0;
}

static void insertSongIntoList(struct songvec *sv, Song *newsong)
{
	Song *existing = songvec_find(sv, newsong->url);

	if (!existing) {
		songvec_add(sv, newsong);
		if (newsong->tag)
			tag_end_add(newsong->tag);
	} else { /* prevent dupes, just update the existing song info */
		if (existing->mtime != newsong->mtime) {
			tag_free(existing->tag);
			if (newsong->tag)
				tag_end_add(newsong->tag);
			existing->tag = newsong->tag;
			existing->mtime = newsong->mtime;
			newsong->tag = NULL;
		}
		freeJustSong(newsong);
	}
}

static int matchesAnMpdTagItemKey(char *buffer, int *itemType)
{
	int i;

	for (i = 0; i < TAG_NUM_OF_ITEM_TYPES; i++) {
		if (!prefixcmp(buffer, mpdTagItemKeys[i])) {
			*itemType = i;
			return 1;
		}
	}

	return 0;
}

void readSongInfoIntoList(FILE * fp, Directory * parentDir)
{
	char buffer[MPD_PATH_MAX + 1024];
	int bufferSize = MPD_PATH_MAX + 1024;
	Song *song = NULL;
	struct songvec *sv = &parentDir->songs;
	int itemType;

	while (myFgets(buffer, bufferSize, fp) && 0 != strcmp(SONG_END, buffer)) {
		if (!prefixcmp(buffer, SONG_KEY)) {
			if (song)
				insertSongIntoList(sv, song);
			song = newNullSong();
			song->url = xstrdup(buffer + strlen(SONG_KEY));
			song->type = SONG_TYPE_FILE;
			song->parentDir = parentDir;
		} else if (*buffer == 0) {
			/* ignore empty lines (starting with '\0') */
		} else if (song == NULL) {
			FATAL("Problems reading song info\n");
		} else if (!prefixcmp(buffer, SONG_FILE)) {
			/* we don't need this info anymore
			   song->url = xstrdup(&(buffer[strlen(SONG_FILE)]));
			 */
		} else if (matchesAnMpdTagItemKey(buffer, &itemType)) {
			if (!song->tag) {
				song->tag = tag_new();
				tag_begin_add(song->tag);
			}

			tag_add_item(song->tag, itemType,
				     &(buffer
				       [strlen(mpdTagItemKeys[itemType]) +
					2]));
		} else if (!prefixcmp(buffer, SONG_TIME)) {
			if (!song->tag) {
				song->tag = tag_new();
				tag_begin_add(song->tag);
			}

			song->tag->time = atoi(&(buffer[strlen(SONG_TIME)]));
		} else if (!prefixcmp(buffer, SONG_MTIME)) {
			song->mtime = atoi(&(buffer[strlen(SONG_MTIME)]));
		}
		else
			FATAL("songinfo: unknown line in db: %s\n", buffer);
	}

	if (song)
		insertSongIntoList(sv, song);
}

int updateSongInfo(Song * song)
{
	if (song->type == SONG_TYPE_FILE) {
		InputPlugin *plugin;
		unsigned int next = 0;
		char path_max_tmp[MPD_PATH_MAX];
		char abs_path[MPD_PATH_MAX];

		utf8_to_fs_charset(abs_path, get_song_url(path_max_tmp, song));
		rmp2amp_r(abs_path, abs_path);

		if (song->tag)
			tag_free(song->tag);

		song->tag = NULL;

		while (!song->tag && (plugin = isMusic(abs_path,
						       &(song->mtime),
						       next++))) {
			song->tag = plugin->tagDupFunc(abs_path);
		}
		if (!song->tag || song->tag->time < 0)
			return -1;
	}

	return 0;
}

char *get_song_url(char *path_max_tmp, Song *song)
{
	if (!song)
		return NULL;

	assert(song->url != NULL);

	if (!song->parentDir || !song->parentDir->path)
		strcpy(path_max_tmp, song->url);
	else
		pfx_dir(path_max_tmp, song->url, strlen(song->url),
			getDirectoryPath(song->parentDir),
			strlen(getDirectoryPath(song->parentDir)));
	return path_max_tmp;
}
