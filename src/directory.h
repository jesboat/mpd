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

#ifndef DIRECTORY_H
#define DIRECTORY_H

#include "song.h"
#include "songvec.h"

struct dirvec {
	struct directory **base;
	size_t nr;
};

struct directory {
	char *path;
	struct dirvec children;
	struct songvec songs;
	struct directory *parent;
	ino_t inode;
	dev_t device;
	unsigned stat; /* not needed if ino_t == dev_t == 0 is impossible */
};

void directory_init(void);

void directory_finish(void);

int isRootDirectory(const char *name);

struct directory * directory_get_root(void);

struct directory * newDirectory(const char *dirname, struct directory *parent);

void freeDirectory(struct directory *directory);

static inline int directory_is_empty(struct directory *directory)
{
	return directory->children.nr == 0 && directory->songs.nr == 0;
}

struct directory * getDirectory(const char *name);

void sortDirectory(struct directory * directory);

int printDirectoryInfo(int fd, const char *dirname);

int checkDirectoryDB(void);

int writeDirectoryDB(void);

int readDirectoryDB(void);

struct mpd_song *getSongFromDB(const char *file);

time_t getDbModTime(void);

int traverseAllIn(const char *name,
		  int (*forEachSong) (struct mpd_song *, void *),
		  int (*forEachDir) (struct directory *, void *), void *data);

#define getDirectoryPath(dir) ((dir && dir->path) ? dir->path : "")

#endif
