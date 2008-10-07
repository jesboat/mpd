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
#include "gcc.h"

#define SONG_KEY	"key: "
#define SONG_MTIME	"mtime: "
#define SONG_BEGIN	"songList begin"
#define SONG_END	"songList end"

#define SONG_FILE	"file: "
#define SONG_TIME	"Time: "

typedef struct _Song {
	struct mpd_tag *tag;
	struct _Directory *parentDir;
	time_t mtime;
	char url[1];
} mpd_packed Song;

Song *newSong(const char *url, struct _Directory *parentDir);

void freeSong(Song *);

void freeJustSong(Song *);

ssize_t song_print_info(Song * song, int fd);

/* like song_print_info, but casts data into an fd first */
int song_print_info_x(Song * song, void *data);

void readSongInfoIntoList(FILE * fp, struct _Directory *parent);

int updateSongInfo(Song * song);

ssize_t song_print_url(Song * song, int fd);

/* like song_print_url_x, but casts data into an fd first */
int song_print_url_x(Song * song, void *data);

/*
 * get_song_url - Returns a path of a song in UTF8-encoded form
 * path_max_tmp is the argument that the URL is written to, this
 * buffer is assumed to be MPD_PATH_MAX or greater (including
 * terminating '\0').
 */
char *get_song_url(char *path_max_tmp, Song * song);

static inline int song_is_file(const Song *song)
{
	return !!song->parentDir;
}

#endif
