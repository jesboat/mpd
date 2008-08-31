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

#define SONG_KEY	"key: "
#define SONG_MTIME	"mtime: "

#include "os_compat.h"

Song *newNullSong(void)
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
		freeMpdTag(song->tag);
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
		printMpdTag(fd, song->tag);

	return 0;
}

int printSongInfoFromList(int fd, SongList * list)
{
	ListNode *tempNode = list->firstNode;

	while (tempNode != NULL) {
		printSongInfo(fd, (Song *) tempNode->data);
		tempNode = tempNode->nextNode;
	}

	return 0;
}

void writeSongInfoFromList(FILE * fp, SongList * list)
{
	ListNode *tempNode = list->firstNode;

	fprintf(fp, "%s\n", SONG_BEGIN);

	while (tempNode != NULL) {
		fprintf(fp, "%s%s\n", SONG_KEY, tempNode->key);
		fflush(fp);
		printSongInfo(fileno(fp), (Song *) tempNode->data);
		fprintf(fp, "%s%li\n", SONG_MTIME,
			  (long)((Song *) tempNode->data)->mtime);
		tempNode = tempNode->nextNode;
	}

	fprintf(fp, "%s\n", SONG_END);
}

static void insertSongIntoList(SongList * list, ListNode ** nextSongNode,
			       char *key, Song * song)
{
	ListNode *nodeTemp;
	int cmpRet = 0;

	while (*nextSongNode
	       && (cmpRet = strcmp(key, (*nextSongNode)->key)) > 0) {
		nodeTemp = (*nextSongNode)->nextNode;
		deleteNodeFromList(list, *nextSongNode);
		*nextSongNode = nodeTemp;
	}

	if (!(*nextSongNode)) {
		insertInList(list, song->url, (void *)song);
	} else if (cmpRet == 0) {
		Song *tempSong = (Song *) ((*nextSongNode)->data);
		if (tempSong->mtime != song->mtime) {
			freeMpdTag(tempSong->tag);
			tempSong->tag = song->tag;
			tempSong->mtime = song->mtime;
			song->tag = NULL;
		}
		freeJustSong(song);
		*nextSongNode = (*nextSongNode)->nextNode;
	} else {
		insertInListBeforeNode(list, *nextSongNode, -1, song->url,
				       (void *)song);
	}
}

static int matchesAnMpdTagItemKey(char *buffer, int *itemType)
{
	int i;

	for (i = 0; i < TAG_NUM_OF_ITEM_TYPES; i++) {
		if (0 == strncmp(mpdTagItemKeys[i], buffer,
				 strlen(mpdTagItemKeys[i]))) {
			*itemType = i;
			return 1;
		}
	}

	return 0;
}

void readSongInfoIntoList(FILE * fp, SongList * list, Directory * parentDir)
{
	char buffer[MPD_PATH_MAX + 1024];
	int bufferSize = MPD_PATH_MAX + 1024;
	Song *song = NULL;
	ListNode *nextSongNode = list->firstNode;
	ListNode *nodeTemp;
	int itemType;

	while (myFgets(buffer, bufferSize, fp) && 0 != strcmp(SONG_END, buffer)) {
		if (0 == strncmp(SONG_KEY, buffer, strlen(SONG_KEY))) {
			if (song)
				insertSongIntoList(list, &nextSongNode,
						   song->url, song);

			song = newNullSong();
			song->url = xstrdup(buffer + strlen(SONG_KEY));
			song->type = SONG_TYPE_FILE;
			song->parentDir = parentDir;
		} else if (*buffer == 0) {
			/* ignore empty lines (starting with '\0') */
		} else if (song == NULL) {
			FATAL("Problems reading song info\n");
		} else if (0 == strncmp(SONG_FILE, buffer, strlen(SONG_FILE))) {
			/* we don't need this info anymore
			   song->url = xstrdup(&(buffer[strlen(SONG_FILE)]));
			 */
		} else if (matchesAnMpdTagItemKey(buffer, &itemType)) {
			if (!song->tag)
				song->tag = newMpdTag();
			addItemToMpdTag(song->tag, itemType,
					&(buffer
					  [strlen(mpdTagItemKeys[itemType]) +
					   2]));
		} else if (0 == strncmp(SONG_TIME, buffer, strlen(SONG_TIME))) {
			if (!song->tag)
				song->tag = newMpdTag();
			song->tag->time = atoi(&(buffer[strlen(SONG_TIME)]));
		} else if (0 == strncmp(SONG_MTIME, buffer, strlen(SONG_MTIME))) {
			song->mtime = atoi(&(buffer[strlen(SONG_MTIME)]));
		}
		else
			FATAL("songinfo: unknown line in db: %s\n", buffer);
	}

	if (song)
		insertSongIntoList(list, &nextSongNode, song->url, song);

	while (nextSongNode) {
		nodeTemp = nextSongNode->nextNode;
		deleteNodeFromList(list, nextSongNode);
		nextSongNode = nodeTemp;
	}
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
			freeMpdTag(song->tag);

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
