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

#include "directory_save.h"
#include "directory.h"
#include "song.h"
#include "log.h"
#include "path.h"
#include "utils.h"
#include "dirvec.h"
#include "myfprintf.h"

static int directory_song_write(struct mpd_song *song, void *data)
{
	int fd = (int)(size_t)data;

	if (fdprintf(fd, SONG_KEY "%s\n", song->url) < 0)
		return -1;
	if (song_print_info(song, fd) < 0)
		return -1;
	if (fdprintf(fd, SONG_MTIME "%li\n", (long)song->mtime) < 0)
		return -1;

	return 0;
}

/* TODO error checking */
int directory_save(int fd, struct directory * directory)
{
	struct dirvec *children = &directory->children;
	size_t i;

	if (!isRootDirectory(directory->path) &&
	    fdprintf(fd, DIRECTORY_BEGIN "%s\n",
	             directory_get_path(directory)) < 0)
		return -1;

	for (i = 0; i < children->nr; ++i) {
		struct directory *cur = children->base[i];
		const char *base = mpd_basename(cur->path);

		if (fdprintf(fd, DIRECTORY_DIR "%s\n", base) < 0)
			return -1;
		if (directory_save(fd, cur) < 0)
			return -1;
	}

	if (fdprintf(fd, SONG_BEGIN "\n") < 0)
		return -1;

	if (songvec_for_each(&directory->songs,
	                     directory_song_write, (void *)(size_t)fd) < 0)
		return -1;

	if (fdprintf(fd, SONG_END "\n") < 0)
		return -1;

	if (!isRootDirectory(directory->path) &&
	    fdprintf(fd, DIRECTORY_END "%s\n",
	             directory_get_path(directory)) < 0)
		return -1;
	return 0;
}

void directory_load(FILE * fp, struct directory * directory)
{
	char buffer[MPD_PATH_MAX * 2];
	int bufferSize = MPD_PATH_MAX * 2;
	char key[MPD_PATH_MAX * 2];
	char *name;

	while (myFgets(buffer, bufferSize, fp)
	       && prefixcmp(buffer, DIRECTORY_END)) {
		if (!prefixcmp(buffer, DIRECTORY_DIR)) {
			struct directory *subdir;

			strcpy(key, &(buffer[strlen(DIRECTORY_DIR)]));
			if (!myFgets(buffer, bufferSize, fp))
				FATAL("Error reading db, fgets\n");
			/* for compatibility with db's prior to 0.11 */
			if (!prefixcmp(buffer, DIRECTORY_MTIME)) {
				if (!myFgets(buffer, bufferSize, fp))
					FATAL("Error reading db, fgets\n");
			}
			if (prefixcmp(buffer, DIRECTORY_BEGIN))
				FATAL("Error reading db at line: %s\n", buffer);
			name = &(buffer[strlen(DIRECTORY_BEGIN)]);
			if (prefixcmp(name, directory->path) != 0)
				FATAL("Wrong path in database: '%s' in '%s'\n",
				      name, directory->path);

			if ((subdir = directory_get_child(directory, name))) {
				assert(subdir->parent == directory);
			} else {
				subdir = directory_new(name, directory);
				dirvec_add(&directory->children, subdir);
			}
			directory_load(fp, subdir);
		} else if (!prefixcmp(buffer, SONG_BEGIN)) {
			readSongInfoIntoList(fp, directory);
		} else {
			FATAL("Unknown line in db: %s\n", buffer);
		}
	}
}
