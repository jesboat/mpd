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

#ifndef METADATA_PIPE_H
#define METADATA_PIPE_H

#include "tag.h"
#include "mpd_types.h"

void init_metadata_pipe(void);

/*
 * Called by the decoder thread, this inserts a tag pointer into the pipe
 * DO NOT FREE the tag placed into the pipe; that is that job of the
 * caller of metadata_pipe_recv() or metadata_pipe_clear().
 */
void metadata_pipe_send(struct mpd_tag * tag, float metadata_time);

/*
 * Reads and consumes one struct mpd_tag pointer off the pipe.  The caller
 * of this MUST free the struct mpd_tag pointer after it is done using it.
 */
struct mpd_tag * metadata_pipe_recv(void);

/*
 * Returns the last read struct mpd_tag from metadata_pipe_recv(), caller
 * must free this pointer when it is done using it.
 */
struct mpd_tag * metadata_pipe_current(void);

/* Clears all struct mpd_tag pointers on the pipe, freeing all associated elements */
void metadata_pipe_clear(void);

#endif /* METADATA_PIPE_H */
