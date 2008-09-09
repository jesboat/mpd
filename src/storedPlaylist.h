/* the Music Player Daemon (MPD)
 * Copyright (C) 2007 by Warren Dukes (warren.dukes@gmail.com)
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

#ifndef STORED_PLAYLIST_H
#define STORED_PLAYLIST_H

#include "song.h"
#include "list.h"
#include "playlist.h"

List *loadStoredPlaylist(const char *utf8path);

enum playlist_result
moveSongInStoredPlaylistByPath(const char *utf8path, int src, int dest);

enum playlist_result
removeAllFromStoredPlaylistByPath(const char *utf8path);

enum playlist_result
removeOneSongFromStoredPlaylistByPath(const char *utf8path, int pos);

enum playlist_result
appendSongToStoredPlaylistByPath(const char *utf8path, Song *song);

enum playlist_result
renameStoredPlaylist(const char *utf8from, const char *utf8to);

#endif
