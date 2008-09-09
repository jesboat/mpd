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

#include "stats.h"

#include "directory.h"
#include "myfprintf.h"
#include "outputBuffer.h"
#include "tag.h"
#include "strset.h"
#include "os_compat.h"
#include "gcc.h"

Stats stats;

void initStats(void)
{
	stats.daemonStart = time(NULL);
	stats.numberOfSongs = 0;
}

struct visit_data {
	enum tag_type type;
	struct strset *set;
};

static int visit_tag_items(Song *song, void *_data)
{
	const struct visit_data *data = _data;
	unsigned i;

	if (song->tag == NULL)
		return 0;

	for (i = 0; i < (unsigned)song->tag->numOfItems; ++i) {
		const struct tag_item *item = song->tag->items[i];
		if (item->type == data->type)
			strset_add(data->set, item->value);
	}

	return 0;
}

static unsigned int getNumberOfTagItems(int type)
{
	struct visit_data data;
	unsigned int ret;

	data.type = type;
	data.set = strset_new();

	traverseAllIn(NULL, visit_tag_items, NULL, &data);

	ret = strset_size(data.set);
	strset_free(data.set);
	return ret;
}

int printStats(int fd)
{
	fdprintf(fd,
		      "artists: %u\n"
		      "albums: %u\n"
		      "songs: %i\n"
		      "uptime: %li\n"
		      "playtime: %li\n"
		      "db_playtime: %li\n"
		      "db_update: %li\n",
		      getNumberOfTagItems(TAG_ITEM_ARTIST),
		      getNumberOfTagItems(TAG_ITEM_ALBUM),
		      stats.numberOfSongs,
		      time(NULL) - stats.daemonStart,
		      (long)(ob_get_total_time() + 0.5),
		      stats.dbPlayTime,
		      getDbModTime());
	return 0;
}
