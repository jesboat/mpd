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
#include "dirvec.h"
#include "songvec.h"
#include "gcc.h"

#define DIRECTORY_DIR		"directory: "
#define DIRECTORY_MTIME		"mtime: " /* DEPRECATED, noop-read-only */
#define DIRECTORY_BEGIN		"begin: "
#define DIRECTORY_END		"end: "
#define DIRECTORY_INFO_BEGIN	"info_begin"
#define DIRECTORY_INFO_END	"info_end"
#define DIRECTORY_MPD_VERSION	"mpd_version: "
#define DIRECTORY_FS_CHARSET	"fs_charset: "

struct directory {
	struct dirvec children;
	struct songvec songs;
	struct directory *parent;
	ino_t inode;
	dev_t device;
	unsigned stat; /* not needed if ino_t == dev_t == 0 is impossible */
	char path[mpd_sizeof_str_flex_array];
} mpd_packed;

extern struct directory music_root;

static inline int path_is_music_root(const char *name)
{
	assert(name);
	return (!*name || !strcmp(name, "/"));
}

struct directory * directory_new(const char *dirname, struct directory *parent);

void directory_free(struct directory *dir);

static inline int directory_is_empty(struct directory *dir)
{
	return dir->children.nr == 0 && dir->songs.nr == 0;
}

static inline const char * directory_get_path(struct directory *dir)
{
	return dir->path;
}

static inline struct directory *
directory_get_child(const struct directory *dir, const char *name)
{
	return dirvec_find(&dir->children, name);
}

static inline struct directory *
directory_new_child(struct directory *dir, const char *name)
{
	struct directory *subdir = directory_new(name, dir);
	dirvec_add(&dir->children, subdir);
	return subdir;
}

void directory_prune_empty(struct directory *dir);

struct directory *
directory_get_subdir(struct directory *dir, const char *name);

int directory_print(int fd, const struct directory *dir);

struct mpd_song *db_get_song(const char *file);

int directory_save(int fd, struct directory *dir);

void directory_load(FILE *fp, struct directory *dir);

int db_walk(const char *name,
		  int (*forEachSong) (struct mpd_song *, void *),
		  int (*forEachDir) (struct directory *, void *), void *data);

int directory_walk(struct directory *dir,
		  int (*forEachSong) (struct mpd_song *, void *),
		  int (*forEachDir) (struct directory *, void *), void *data);

#endif
