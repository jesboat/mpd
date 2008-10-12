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

enum update_return {
	UPDATE_RETURN_ERROR = -1,
	UPDATE_RETURN_NOUPDATE = 0,
	UPDATE_RETURN_UPDATED = 1
};

enum update_progress {
	UPDATE_PROGRESS_IDLE = 0,
	UPDATE_PROGRESS_RUNNING = 1,
	UPDATE_PROGRESS_DONE = 2
} progress;

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
	struct directory *directory = data;
	assert(song->parent == directory);
	delete_song(directory, song);
	return 0;
}

/**
 * Recursively remove all sub directories and songs from a directory,
 * leaving an empty directory.
 */
static void clear_directory(struct directory *directory)
{
	int i;

	for (i = directory->children.nr; --i >= 0;)
		clear_directory(directory->children.base[i]);
	dirvec_clear(&directory->children);

	songvec_for_each(&directory->songs, delete_each_song, directory);
}

/**
 * Recursively free a directory and all its contents.
 */
static void delete_directory(struct directory *directory)
{
	assert(directory->parent != NULL);

	clear_directory(directory);

	dirvec_delete(&directory->parent->children, directory);
	directory_free(directory);
}

struct delete_data {
	char *tmp;
	struct directory *dir;
	enum update_return ret;
};

/* passed to songvec_for_each */
static int delete_song_if_removed(struct mpd_song *song, void *_data)
{
	struct delete_data *data = _data;

	data->tmp = song_get_url(song, data->tmp);
	assert(data->tmp);

	if (!isFile(data->tmp, NULL)) {
		delete_song(data->dir, song);
		data->ret = UPDATE_RETURN_UPDATED;
	}
	return 0;
}

static enum update_return delete_path(const char *path)
{
	struct directory *directory = db_get_directory(path);
	struct mpd_song *song = db_get_song(path);

	if (directory)
		delete_directory(directory);
	if (song)
		delete_song(song->parent, song);

	return directory == NULL && song == NULL
		? UPDATE_RETURN_NOUPDATE
		: UPDATE_RETURN_UPDATED;
}

static enum update_return
removeDeletedFromDirectory(char *path_max_tmp, struct directory *directory)
{
	enum update_return ret = UPDATE_RETURN_NOUPDATE;
	int i;
	struct dirvec *dv = &directory->children;
	struct delete_data data;

	for (i = dv->nr; --i >= 0; ) {
		if (isDir(dv->base[i]->path))
			continue;
		LOG("removing directory: %s\n", dv->base[i]->path);
		dirvec_delete(dv, dv->base[i]);
		ret = UPDATE_RETURN_UPDATED;
	}

	data.dir = directory;
	data.tmp = path_max_tmp;
	data.ret = ret;
	songvec_for_each(&directory->songs, delete_song_if_removed, &data);

	return data.ret;
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

static enum update_return
updateDirectory(struct directory *directory, const struct stat *st);

static enum update_return
updateInDirectory(struct directory *directory,
		  const char *name, const struct stat *st)
{
	if (S_ISREG(st->st_mode) && hasMusicSuffix(name, 0)) {
		const char *shortname = mpd_basename(name);
		struct mpd_song *song;

		if (!(song = songvec_find(&directory->songs, shortname))) {
			if (!(song = song_file_load(shortname, directory)))
				return -1;
			songvec_add(&directory->songs, song);
			LOG("added %s\n", name);
			return UPDATE_RETURN_UPDATED;
		} else if (st->st_mtime != song->mtime) {
			LOG("updating %s\n", name);
			if (!song_file_update(song))
				delete_song(directory, song);
			return UPDATE_RETURN_UPDATED;
		}

		return UPDATE_RETURN_NOUPDATE;
	} else if (S_ISDIR(st->st_mode)) {
		struct directory *subdir;
		enum update_return ret;

		if (inodeFoundInParent(directory, st->st_ino, st->st_dev))
			return UPDATE_RETURN_ERROR;

		if (!(subdir = directory_get_child(directory, name)))
			subdir = directory_new_child(directory, name);

		assert(directory == subdir->parent);

		ret = updateDirectory(subdir, st);
		if (ret == UPDATE_RETURN_ERROR || directory_is_empty(subdir))
			delete_directory(subdir);

		return ret;
	} else {
		DEBUG("update: %s is not a directory or music\n", name);
		return UPDATE_RETURN_NOUPDATE;
	}
}

/* we don't look at hidden files nor files with newlines in them */
static int skip_path(const char *path)
{
	return (path[0] == '.' || strchr(path, '\n')) ? 1 : 0;
}

static enum update_return
updateDirectory(struct directory *directory, const struct stat *st)
{
	DIR *dir;
	const char *dirname = directory_get_path(directory);
	struct dirent *ent;
	char path_max_tmp[MPD_PATH_MAX];
	enum update_return ret = UPDATE_RETURN_NOUPDATE;
	enum update_return ret2;

	assert(S_ISDIR(st->st_mode));

	directory_set_stat(directory, st);

	dir = opendir(opendir_path(path_max_tmp, dirname));
	if (!dir)
		return UPDATE_RETURN_ERROR;

	if (removeDeletedFromDirectory(path_max_tmp, directory) > 0)
		ret = UPDATE_RETURN_UPDATED;

	while ((ent = readdir(dir))) {
		char *utf8;
		struct stat st2;

		if (skip_path(ent->d_name))
			continue;

		utf8 = fs_charset_to_utf8(path_max_tmp, ent->d_name);
		if (!utf8)
			continue;

		if (!isRootDirectory(directory->path))
			utf8 = pfx_dir(path_max_tmp, utf8, strlen(utf8),
			               dirname, strlen(dirname));

		if (myStat(path_max_tmp, &st2) == 0)
			ret2 = updateInDirectory(directory, path_max_tmp, &st2);
		else
			ret2 = delete_path(path_max_tmp);
		if (ret == UPDATE_RETURN_NOUPDATE)
			ret = ret2;
	}

	closedir(dir);

	return ret;
}

static struct directory *
directory_make_child_checked(struct directory *parent, const char *path)
{
	struct directory *directory;
	struct stat st;
	struct mpd_song *conflicting;

	if ((directory = directory_get_child(parent, path)))
		return directory;

	if (myStat(path, &st) < 0 ||
	    inodeFoundInParent(parent, st.st_ino, st.st_dev))
		return NULL;

	/* if we're adding directory paths, make sure to delete filenames
	   with potentially the same name */
	if ((conflicting = songvec_find(&parent->songs, mpd_basename(path))))
		delete_song(parent, conflicting);

	directory = directory_new_child(parent, path);
	directory_set_stat(directory, &st);
	return directory;
}

static struct directory *
addParentPathToDB(const char *utf8path)
{
	struct directory *directory = db_get_root();
	char *duplicated = xstrdup(utf8path);
	char *slash = duplicated;

	while ((slash = strchr(slash, '/'))) {
		*slash = 0;

		directory = directory_make_child_checked(directory, duplicated);
		if (!directory || !slash)
			break;

		*slash++ = '/';
	}

	free(duplicated);
	return directory;
}

static enum update_return updatePath(const char *utf8path)
{
	struct stat st;

	if (myStat(utf8path, &st) < 0)
		return delete_path(utf8path);
	return updateInDirectory(addParentPathToDB(utf8path), utf8path, &st);
}

static void * update_task(void *_path)
{
	enum update_return ret = UPDATE_RETURN_NOUPDATE;

	if (_path != NULL && !isRootDirectory(_path)) {
		ret = updatePath((char *)_path);
		free(_path);
	} else {
		struct directory *directory = db_get_root();
		struct stat st;

		if (myStat(directory_get_path(directory), &st) == 0)
			ret = updateDirectory(directory, &st);
		else
			ret = UPDATE_RETURN_ERROR;
	}

	if (ret == UPDATE_RETURN_UPDATED && db_save() < 0)
		ret = UPDATE_RETURN_ERROR;
	progress = UPDATE_PROGRESS_DONE;
	wakeup_main_task();
	return (void *)ret;
}

static void spawn_update_task(char *path)
{
	pthread_attr_t attr;

	assert(pthread_equal(pthread_self(), main_task));

	progress = UPDATE_PROGRESS_RUNNING;
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

	if (progress != UPDATE_PROGRESS_IDLE) {
		unsigned next_task_id;

		if (!path)
			return 0;
		if (update_paths_nr == ARRAY_SIZE(update_paths)) {
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
	void *thread_return;
	enum update_return ret;

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
	if (pthread_join(update_thr, &thread_return))
		FATAL("error joining update thread: %s\n", strerror(errno));
	ret = (enum update_return)(size_t)thread_return;
	if (ret == UPDATE_RETURN_UPDATED)
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
