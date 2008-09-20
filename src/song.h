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

#ifndef SONG_H
#define SONG_H

#include "../config.h"
#include "os_compat.h"
#include "tag.h"
#include "list.h"

#define SONG_KEY	"key: "
#define SONG_MTIME	"mtime: "
#define SONG_BEGIN	"songList begin"
#define SONG_END	"songList end"

#define SONG_TYPE_FILE 1
#define SONG_TYPE_URL 2

#define SONG_FILE	"file: "
#define SONG_TIME	"Time: "

typedef struct _Song {
	char *url;
	mpd_sint8 type;
	struct mpd_tag *tag;
	struct _Directory *parentDir;
	time_t mtime;
} Song;

typedef List SongList;

Song *newSong(const char *url, int songType, struct _Directory *parentDir);

void freeSong(Song *);

void freeJustSong(Song *);

SongList *newSongList(void);

void freeSongList(SongList * list);

Song *addSongToList(SongList * list, const char *url, const char *utf8path,
		    int songType, struct _Directory *parentDir);

int printSongInfo(int fd, Song * song);

void readSongInfoIntoList(FILE * fp, struct _Directory *parent);

int updateSongInfo(Song * song);

void printSongUrl(int fd, Song * song);

/*
 * get_song_url - Returns a path of a song in UTF8-encoded form
 * path_max_tmp is the argument that the URL is written to, this
 * buffer is assumed to be MPD_PATH_MAX or greater (including
 * terminating '\0').
 */
char *get_song_url(char *path_max_tmp, Song * song);

#endif
