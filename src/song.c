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

#include "os_compat.h"

static struct mpd_song * song_alloc(const char *url, struct directory *parent)
{
	size_t urllen;
	struct mpd_song *song;

	assert(url);
	urllen = strlen(url);
	assert(urllen);
	song = xmalloc(sizeof(*song) - sizeof(song->url) + urllen + 1);

	song->tag = NULL;
	memcpy(song->url, url, urllen + 1);
	song->parent = parent;

	return song;
}

struct mpd_song * song_remote_new(const char *url)
{
	return song_alloc(url, NULL);
}

struct mpd_song * song_file_new(const char *path, struct directory *parent)
{
	assert(parent != NULL);

	return song_alloc(path, parent);
}

struct mpd_song * song_file_load(const char *path, struct directory *parent)
{
	struct mpd_song *song;

	if (strchr(path, '\n')) {
		DEBUG("song_file_load: '%s' is not a valid uri\n", path);
		return NULL;
	}

	song = song_file_new(path, parent);
	if (!song_file_update(song)) {
		song_free(song);
		return NULL;
	}

	return song;
}

void song_free(struct mpd_song * song)
{
	if (song->tag)
		tag_free(song->tag);
	free(song);
}

ssize_t song_print_url(struct mpd_song *song, int fd)
{
	if (song->parent && song->parent != &music_root)
		return fdprintf(fd, "%s%s/%s\n", SONG_FILE,
			        directory_get_path(song->parent), song->url);
	return fdprintf(fd, "%s%s\n", SONG_FILE, song->url);
}

ssize_t song_print_info(struct mpd_song *song, int fd)
{
	ssize_t ret = song_print_url(song, fd);

	if (ret < 0)
		return ret;
	if (song->tag)
		tag_print(fd, song->tag);

	return ret;
}

int song_print_info_x(struct mpd_song * song, void *data)
{
	return song_print_info(song, (int)(size_t)data);
}

int song_print_url_x(struct mpd_song * song, void *data)
{
	return song_print_url(song, (int)(size_t)data);
}

static void insertSongIntoList(struct songvec *sv, struct mpd_song *newsong)
{
	struct mpd_song *existing = songvec_find(sv, newsong->url);

	if (!existing) {
		songvec_add(sv, newsong);
		if (newsong->tag)
			tag_end_add(newsong->tag);
	} else { /* prevent dupes, just update the existing song info */
		if (existing->mtime != newsong->mtime) {
			existing->mtime = newsong->mtime;
			if (tag_equal(existing->tag, newsong->tag)) {
				if (newsong->tag)
					tag_free(newsong->tag);
			} else {
				struct mpd_tag *old_tag = existing->tag;

				if (newsong->tag)
					tag_end_add(newsong->tag);
				existing->tag = newsong->tag;
				if (old_tag)
					tag_free(old_tag);
			}
			/* prevent tag_free in song_free */
			newsong->tag = NULL;
		}
		song_free(newsong);
	}
}

static int matchesAnMpdTagItemKey(char *buffer, int *itemType)
{
	int i;

	for (i = 0; i < TAG_NUM_OF_ITEM_TYPES; i++) {
		if (!prefixcmp(buffer, mpdTagItemKeys[i])) {
			*itemType = i;
			return 1;
		}
	}

	return 0;
}

void readSongInfoIntoList(FILE * fp, struct directory * parent)
{
	char buffer[MPD_PATH_MAX + 1024];
	int bufferSize = MPD_PATH_MAX + 1024;
	struct mpd_song *song = NULL;
	struct songvec *sv = &parent->songs;
	int itemType;

	while (myFgets(buffer, bufferSize, fp) && 0 != strcmp(SONG_END, buffer)) {
		if (!prefixcmp(buffer, SONG_KEY)) {
			if (song)
				insertSongIntoList(sv, song);
			song = song_file_new(buffer + strlen(SONG_KEY), parent);
		} else if (*buffer == 0) {
			/* ignore empty lines (starting with '\0') */
		} else if (song == NULL) {
			FATAL("Problems reading song info\n");
		} else if (!prefixcmp(buffer, SONG_FILE)) {
			/* we don't need this info anymore */
		} else if (matchesAnMpdTagItemKey(buffer, &itemType)) {
			if (!song->tag) {
				song->tag = tag_new();
				tag_begin_add(song->tag);
			}

			tag_add_item(song->tag, itemType,
				     &(buffer
				       [strlen(mpdTagItemKeys[itemType]) +
					2]));
		} else if (!prefixcmp(buffer, SONG_TIME)) {
			if (!song->tag) {
				song->tag = tag_new();
				tag_begin_add(song->tag);
			}

			song->tag->time = atoi(&(buffer[strlen(SONG_TIME)]));
		} else if (!prefixcmp(buffer, SONG_MTIME)) {
			song->mtime = atoi(&(buffer[strlen(SONG_MTIME)]));
		}
		else
			FATAL("songinfo: unknown line in db: %s\n", buffer);
	}

	if (song)
		insertSongIntoList(sv, song);
}

int song_file_update(struct mpd_song * song)
{
	InputPlugin *plugin;
	unsigned int next = 0;
	char path_max_tmp[MPD_PATH_MAX];
	char abs_path[MPD_PATH_MAX];
	struct mpd_tag *old_tag = song->tag;
	struct mpd_tag *new_tag = NULL;

	assert(song_is_file(song));

	utf8_to_fs_charset(abs_path, song_get_url(song, path_max_tmp));
	rmp2amp_r(abs_path, abs_path);

	while ((plugin = isMusic(abs_path, &song->mtime, next++))) {
		if ((new_tag = plugin->tagDupFunc(abs_path)))
			break;
	}
	if (new_tag && tag_equal(new_tag, old_tag)) {
		tag_free(new_tag);
	} else {
		song->tag = new_tag;
		if (old_tag)
			tag_free(old_tag);
	}

	return (song->tag && song->tag->time >= 0);
}

char *song_get_url(struct mpd_song *song, char *path_max_tmp)
{
	if (!song)
		return NULL;

	assert(*song->url);

	if (!song->parent || song->parent == &music_root)
		strcpy(path_max_tmp, song->url);
	else
		pfx_dir(path_max_tmp, song->url, strlen(song->url),
			directory_get_path(song->parent),
			strlen(directory_get_path(song->parent)));
	return path_max_tmp;
}
