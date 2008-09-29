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
#include "list.h"

struct dirvec {
	struct _Directory **base;
	size_t nr;
};

typedef struct _Directory {
	char *path;
	struct dirvec children;
	struct songvec songs;
	struct _Directory *parent;
	ino_t inode;
	dev_t device;
	unsigned stat; /* not needed if ino_t == dev_t == 0 is impossible */
} Directory;

void reap_update_task(void);

int isUpdatingDB(void);

/* returns the non-negative update job ID on success, -1 on error */
int updateInit(List * pathList);

void directory_init(void);

void directory_finish(void);

int isRootDirectory(const char *name);

int printDirectoryInfo(int fd, const char *dirname);

int checkDirectoryDB(void);

int writeDirectoryDB(void);

int readDirectoryDB(void);

Song *getSongFromDB(const char *file);

time_t getDbModTime(void);

int traverseAllIn(const char *name,
		  int (*forEachSong) (Song *, void *),
		  int (*forEachDir) (Directory *, void *), void *data);

#define getDirectoryPath(dir) ((dir && dir->path) ? dir->path : "")

#endif
