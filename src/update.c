/* the Music Player Daemon (MPD)
 * Copyright (C) 2003-2007 by Warren Dukes (warren.dukes@gmail.com)
 * Copyright (C) 2008 Max Kellermann <max@duempel.org>
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

#include "update.h"
#include "database.h"
#include "log.h"
#include "ls.h"
#include "path.h"
#include "playlist.h"
#include "utils.h"
#include "main_notify.h"
#include "condition.h"
#include "update.h"

static enum update_progress {
	UPDATE_PROGRESS_IDLE = 0,
	UPDATE_PROGRESS_RUNNING = 1,
	UPDATE_PROGRESS_DONE = 2
} progress;

static int modified;

/* make this dynamic?, or maybe this is big enough... */
static char *update_paths[32];
static size_t update_paths_nr;

static pthread_t update_thr;

static const unsigned update_task_id_max = 1 << 15;

static unsigned update_task_id;

static struct mpd_song *delete;

static struct condition delete_cond;

unsigned isUpdatingDB(void)
{
	return (progress != UPDATE_PROGRESS_IDLE) ? update_task_id : 0;
}

static void directory_set_stat(struct directory *dir, const struct stat *st)
{
	dir->inode = st->st_ino;
	dir->device = st->st_dev;
	dir->stat = 1;
}

static void delete_song(struct directory *dir, struct mpd_song *del)
{
	/* first, prevent traversers in main task from getting this */
	songvec_delete(&dir->songs, del);

	/* now take it out of the playlist (in the main_task) */
	cond_enter(&delete_cond);
	assert(!delete);
	delete = del;
	wakeup_main_task();
	do { cond_wait(&delete_cond); } while (delete);
	cond_leave(&delete_cond);

	/* finally, all possible references gone, free it */
	song_free(del);
}

static int delete_each_song(struct mpd_song *song, mpd_unused void *data)
{
	struct directory *dir = data;
	assert(song->parent == dir);
	delete_song(dir, song);
	return 0;
}

/**
 * Recursively remove all sub directories and songs from a directory,
 * leaving an empty directory.
 */
static int clear_directory(struct directory *dir, mpd_unused void *arg)
{
	dirvec_for_each(&dir->children, clear_directory, NULL);
	dirvec_clear(&dir->children);
	songvec_for_each(&dir->songs, delete_each_song, dir);
	return 0;
}

/**
 * Recursively free a directory and all its contents.
 */
static void delete_directory(struct directory *dir)
{
	assert(dir->parent != NULL);

	clear_directory(dir, NULL);
	dirvec_delete(&dir->parent->children, dir);
	directory_free(dir);
}

struct delete_data {
	char *tmp;
	struct directory *dir;
};

/* passed to songvec_for_each */
static int delete_song_if_removed(struct mpd_song *song, void *_data)
{
	struct delete_data *data = _data;

	data->tmp = song_get_url(song, data->tmp);
	assert(data->tmp);

	if (!isFile(data->tmp, NULL)) {
		delete_song(data->dir, song);
		modified = 1;
	}
	return 0;
}

static void delete_path(const char *path)
{
	struct directory *dir = db_get_directory(path);
	struct mpd_song *song = db_get_song(path);

	if (dir) {
		delete_directory(dir);
		modified = 1;
	}
	if (song) {
		delete_song(song->parent, song);
		modified = 1;
	}
}

/* passed to dirvec_for_each */
static int delete_directory_if_removed(struct directory *dir, void *_data)
{
	if (!isDir(dir->path)) {
		struct delete_data *data = _data;

		LOG("removing directory: %s\n", directory_get_path(dir));
		dirvec_delete(&data->dir->children, dir);
		modified = 1;
	}
	return 0;
}

static void
removeDeletedFromDirectory(char *path_max_tmp, struct directory *dir)
{
	struct delete_data data;

	data.dir = dir;
	data.tmp = path_max_tmp;
	dirvec_for_each(&dir->children, delete_directory_if_removed, &data);
	songvec_for_each(&dir->songs, delete_song_if_removed, &data);
}

static const char *opendir_path(char *path_max_tmp, const char *dirname)
{
	if (*dirname != '\0')
		return rmp2amp_r(path_max_tmp,
		                 utf8_to_fs_charset(path_max_tmp, dirname));
	return musicDir;
}

static int statDirectory(struct directory *dir)
{
	struct stat st;

	if (myStat(directory_get_path(dir), &st) < 0)
		return -1;

	directory_set_stat(dir, &st);

	return 0;
}

static int
inodeFoundInParent(struct directory *parent, ino_t inode, dev_t device)
{
	while (parent) {
		if (!parent->stat && statDirectory(parent) < 0)
			return -1;
		if (parent->inode == inode && parent->device == device) {
			DEBUG("recursive directory found\n");
			return 1;
		}
		parent = parent->parent;
	}

	return 0;
}

static int updateDirectory(struct directory *dir, const struct stat *st);

static void
updateInDirectory(struct directory *dir,
		  const char *name, const struct stat *st)
{
	if (S_ISREG(st->st_mode) && hasMusicSuffix(name, 0)) {
		const char *shortname = mpd_basename(name);
		struct mpd_song *song;

		if (!(song = songvec_find(&dir->songs, shortname))) {
			if (!(song = song_file_load(shortname, dir)))
				return;
			songvec_add(&dir->songs, song);
			modified = 1;
			LOG("added %s\n", name);
		} else if (st->st_mtime != song->mtime) {
			LOG("updating %s\n", name);
			if (!song_file_update(song))
				delete_song(dir, song);
			modified = 1;
		}
	} else if (S_ISDIR(st->st_mode)) {
		struct directory *subdir;

		if (inodeFoundInParent(dir, st->st_ino, st->st_dev))
			return;

		if (!(subdir = directory_get_child(dir, name)))
			subdir = directory_new_child(dir, name);

		assert(dir == subdir->parent);

		if (!updateDirectory(subdir, st))
			delete_directory(subdir);
	} else {
		DEBUG("update: %s is not a directory or music\n", name);
	}
}

/* we don't look at hidden files nor files with newlines in them */
static int skip_path(const char *path)
{
	return (path[0] == '.' || strchr(path, '\n')) ? 1 : 0;
}

static int updateDirectory(struct directory *dir, const struct stat *st)
{
	DIR *fs_dir;
	const char *dirname = directory_get_path(dir);
	struct dirent *ent;
	char path_max_tmp[MPD_PATH_MAX];

	assert(S_ISDIR(st->st_mode));

	directory_set_stat(dir, st);

	if (!(fs_dir = opendir(opendir_path(path_max_tmp, dirname))))
		return 0;

	removeDeletedFromDirectory(path_max_tmp, dir);

	while ((ent = readdir(fs_dir))) {
		char *utf8;
		struct stat st2;

		if (skip_path(ent->d_name))
			continue;

		utf8 = fs_charset_to_utf8(path_max_tmp, ent->d_name);
		if (!utf8)
			continue;

		if (dir != &music_root)
			utf8 = pfx_dir(path_max_tmp, utf8, strlen(utf8),
			               dirname, strlen(dirname));

		if (myStat(path_max_tmp, &st2) == 0)
			updateInDirectory(dir, path_max_tmp, &st2);
		else
			delete_path(path_max_tmp);
	}

	closedir(fs_dir);

	return 1;
}

static struct directory *
directory_make_child_checked(struct directory *parent, const char *path)
{
	struct directory *dir;
	struct stat st;
	struct mpd_song *conflicting;

	if ((dir = directory_get_child(parent, path)))
		return dir;

	if (myStat(path, &st) < 0 ||
	    inodeFoundInParent(parent, st.st_ino, st.st_dev))
		return NULL;

	/* if we're adding directory paths, make sure to delete filenames
	   with potentially the same name */
	if ((conflicting = songvec_find(&parent->songs, mpd_basename(path))))
		delete_song(parent, conflicting);

	dir = directory_new_child(parent, path);
	directory_set_stat(dir, &st);
	return dir;
}

static struct directory * addParentPathToDB(const char *utf8path)
{
	struct directory *dir = &music_root;
	char *duplicated = xstrdup(utf8path);
	char *slash = duplicated;

	while ((slash = strchr(slash, '/'))) {
		*slash = 0;

		dir = directory_make_child_checked(dir, duplicated);
		if (!dir || !slash)
			break;

		*slash++ = '/';
	}

	free(duplicated);
	return dir;
}

static void updatePath(const char *utf8path)
{
	struct stat st;

	if (myStat(utf8path, &st) < 0)
		delete_path(utf8path);
	else
		updateInDirectory(addParentPathToDB(utf8path), utf8path, &st);
}

static void * update_task(void *_path)
{
	char *utf8path = _path;

	if (utf8path) {
		assert(*utf8path);
		updatePath(utf8path);
		free(utf8path);
	} else {
		struct stat st;

		if (myStat(directory_get_path(&music_root), &st) == 0)
			updateDirectory(&music_root, &st);
	}

	if (modified)
		db_save();
	progress = UPDATE_PROGRESS_DONE;
	wakeup_main_task();
	return NULL;
}

static void spawn_update_task(char *path)
{
	pthread_attr_t attr;

	assert(pthread_equal(pthread_self(), main_task));

	progress = UPDATE_PROGRESS_RUNNING;
	modified = 0;
	pthread_attr_init(&attr);
	if (pthread_create(&update_thr, &attr, update_task, path))
		FATAL("Failed to spawn update task: %s\n", strerror(errno));
	if (++update_task_id > update_task_id_max)
		update_task_id = 1;
	DEBUG("spawned thread for update job id %i\n", update_task_id);
}

unsigned directory_update_init(char *path)
{
	assert(pthread_equal(pthread_self(), main_task));

	assert(!path || (path && *path));

	if (progress != UPDATE_PROGRESS_IDLE) {
		unsigned next_task_id;

		if (update_paths_nr == ARRAY_SIZE(update_paths)) {
			if (path)
				free(path);
			return 0;
		}

		assert(update_paths_nr < ARRAY_SIZE(update_paths));
		update_paths[update_paths_nr++] = path;
		next_task_id = update_task_id + update_paths_nr;

		return next_task_id > update_task_id_max ?  1 : next_task_id;
	}
	spawn_update_task(path);
	return update_task_id;
}

void reap_update_task(void)
{
	assert(pthread_equal(pthread_self(), main_task));

	if (progress == UPDATE_PROGRESS_IDLE)
		return;

	cond_enter(&delete_cond);
	if (delete) {
		char tmp[MPD_PATH_MAX];
		LOG("removing: %s\n", song_get_url(delete, tmp));
		deleteASongFromPlaylist(delete);
		delete = NULL;
		cond_signal(&delete_cond);
	}
	cond_leave(&delete_cond);

	if (progress != UPDATE_PROGRESS_DONE)
		return;
	if (pthread_join(update_thr, NULL))
		FATAL("error joining update thread: %s\n", strerror(errno));

	if (modified)
		playlistVersionChange();

	if (update_paths_nr) {
		char *path = update_paths[0];
		memmove(&update_paths[0], &update_paths[1],
		        --update_paths_nr * sizeof(char *));
		spawn_update_task(path);
	} else {
		progress = UPDATE_PROGRESS_IDLE;
	}
}
