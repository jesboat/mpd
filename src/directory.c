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

#include "directory.h"

#include "utils.h"
#include "ack.h"
#include "myfprintf.h"
#include "dirvec.h"

struct directory music_root;

struct directory * directory_new(const char *path, struct directory * parent)
{
	struct directory *dir;
	size_t pathlen;

	assert(path);
	assert(*path);
	assert(parent);

	pathlen = strlen(path);
	dir = xcalloc(1, sizeof(*dir) - sizeof(dir->path) + pathlen + 1);
	memcpy(dir->path, path, pathlen + 1);
	dir->parent = parent;

	return dir;
}

void directory_free(struct directory *dir)
{
	dirvec_destroy(&dir->children);
	songvec_destroy(&dir->songs);
	if (dir != &music_root)
		free(dir);
}

static int dir_pruner(struct directory *dir, void *_dv)
{
	directory_prune_empty(dir);
	if (directory_is_empty(dir))
		dirvec_delete((struct dirvec *)_dv, dir);
	return 0;
}

void directory_prune_empty(struct directory *dir)
{
	dirvec_for_each(&dir->children, dir_pruner, &dir->children);
}

struct directory *
directory_get_subdir(struct directory *dir, const char *name)
{
	struct directory *cur = dir;
	struct directory *found = NULL;
	char *duplicated;
	char *locate;

	assert(name != NULL);

	if (path_is_music_root(name))
		return dir;

	duplicated = xstrdup(name);
	locate = strchr(duplicated, '/');
	while (1) {
		if (locate)
			*locate = '\0';
		if (!(found = directory_get_child(cur, duplicated)))
			break;
		assert(cur == found->parent);
		cur = found;
		if (!locate)
			break;
		*locate = '/';
		locate = strchr(locate + 1, '/');
	}

	free(duplicated);

	return found;
}

static int directory_sort_x(struct directory *dir, mpd_unused void *arg)
{
	directory_sort(dir);
	return 0;
}

void directory_sort(struct directory *dir)
{
	dirvec_sort(&dir->children);
	dirvec_for_each(&dir->children, directory_sort_x, NULL);
	songvec_sort(&dir->songs);
}

struct dirwalk_arg {
	int (*each_song) (struct mpd_song *, void *);
	int (*each_dir) (struct directory *, void *);
	void *data;
};

static int dirwalk_x(struct directory *dir, void *_arg)
{
	struct dirwalk_arg *arg = _arg;

	return directory_walk(dir, arg->each_song, arg->each_dir, arg->data);
}

int directory_walk(struct directory *dir,
			  int (*forEachSong) (struct mpd_song *, void *),
			  int (*forEachDir) (struct directory *, void *),
			  void *data)
{
	int err = 0;

	if (forEachDir && (err = forEachDir(dir, data)) < 0)
		return err;

	if (forEachSong) {
		err = songvec_for_each(&dir->songs, forEachSong, data);
		if (err < 0)
			return err;
	}

	if (forEachDir) {
		struct dirwalk_arg dw_arg;

		dw_arg.each_song = forEachSong;
		dw_arg.each_dir = forEachDir;
		dw_arg.data = data;
		err = dirvec_for_each(&dir->children, dirwalk_x, &dw_arg);
	}
	return err;
}
