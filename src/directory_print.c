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

#include "directory_print.h"
#include "directory.h"
#include "dirvec.h"
#include "songvec.h"
#include "myfprintf.h"

static int dirvec_print(int fd, const struct dirvec *dv)
{
	size_t i;

	for (i = 0; i < dv->nr; ++i) {
		if (fdprintf(fd, DIRECTORY_DIR "%s\n",
		             directory_get_path(dv->base[i])) < 0)
			return -1;
	}

	return 0;
}

int directory_print(int fd, const struct directory *directory)
{
	if (dirvec_print(fd, &directory->children) < 0)
		return -1;
	if (songvec_for_each(&directory->songs, song_print_info_x,
	                     (void *)(size_t)fd) < 0)
		return -1;
	return 0;
}
