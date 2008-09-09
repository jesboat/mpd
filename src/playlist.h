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

#ifndef PLAYLIST_H
#define PLAYLIST_H

#include "locate.h"

#define PLAYLIST_FILE_SUFFIX 	"m3u"
#define PLAYLIST_COMMENT	'#'

extern int playlist_saveAbsolutePaths;

extern int playlist_max_length;

void initPlaylist(void);

void finishPlaylist(void);

void readPlaylistState(FILE *);

void savePlaylistState(FILE *);

void clearPlaylist(void);

int clearStoredPlaylist(int fd, const char *utf8file);

int addToPlaylist(int fd, const char *file, int *added_id);

int addToStoredPlaylist(int fd, const char *file, const char *utf8file);

int addSongToPlaylist(int fd, Song * song, int *added_id);

void showPlaylist(int fd);

int deleteFromPlaylist(int fd, int song);

int deleteFromPlaylistById(int fd, int song);

int playlistInfo(int fd, int song);

int playlistId(int fd, int song);

Song *playlist_queued_song(void);

void playlist_queue_next(void);

int playlist_playing(void);

void stopPlaylist(void);

int playPlaylist(int fd, int song, int stopOnError);

int playPlaylistById(int fd, int song, int stopOnError);

void nextSongInPlaylist(void);

void syncPlayerAndPlaylist(void);

void previousSongInPlaylist(void);

void shufflePlaylist(int fd);

int savePlaylist(int fd, const char *utf8file);

int deletePlaylist(int fd, const char *utf8file);

int deletePlaylistById(int fd, const char *utf8file);

void deleteASongFromPlaylist(Song * song);

int moveSongInPlaylist(int fd, int from, int to);

int moveSongInPlaylistById(int fd, int id, int to);

int swapSongsInPlaylist(int fd, int song1, int song2);

int swapSongsInPlaylistById(int fd, int id1, int id2);

int loadPlaylist(int fd, const char *utf8file);

int getPlaylistRepeatStatus(void);

void setPlaylistRepeatStatus(int status);

int getPlaylistRandomStatus(void);

void setPlaylistRandomStatus(int status);

int getPlaylistCurrentSong(void);

int getPlaylistSongId(int song);

int getPlaylistLength(void);

unsigned long getPlaylistVersion(void);

int seekSongInPlaylist(int fd, int song, float seek_time);

int seekSongInPlaylistById(int fd, int id, float seek_time);

void playlistVersionChange(void);

int playlistChanges(int fd, mpd_uint32 version);

int playlistChangesPosId(int fd, mpd_uint32 version);

int PlaylistInfo(int fd, const char *utf8file, int detail);

void searchForSongsInPlaylist(int fd, int numItems, LocateTagItem * items);

void findSongsInPlaylist(int fd, int numItems, LocateTagItem * items);

int is_valid_playlist_name(const char *utf8path);

int valid_playlist_name(int err_fd, const char *utf8path);

struct mpd_tag *playlist_current_tag(void);

#endif
