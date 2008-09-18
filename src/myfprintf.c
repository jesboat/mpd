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

#include "myfprintf.h"
#include "client.h"
#include "path.h"
#include "utils.h"
#include "os_compat.h"

#define BUFFER_LENGTH	MPD_PATH_MAX+1024

static ssize_t blocking_write(int fd, const char *string, size_t len)
{
	const char *base = string;

	while (len) {
		ssize_t ret = xwrite(fd, string, len);
		if (ret < 0)
			return -1;
		if (!ret) {
			errno = ENOSPC;
			return -1;
		}
		len -= ret;
		string += ret;
	}
	return string - base;
}

ssize_t vfdprintf(const int fd, const char *fmt, va_list args)
{
	char buffer[BUFFER_LENGTH];
	char *buf = buffer;
	size_t len;

	vsnprintf(buf, BUFFER_LENGTH, fmt, args);
	len = strlen(buf);
	if (fd == STDERR_FILENO ||
	    fd == STDOUT_FILENO ||
	    client_print(fd, buf, len) < 0)
		return blocking_write(fd, buf, len);
	return len;
}

mpd_fprintf ssize_t fdprintf(const int fd, const char *fmt, ...)
{
	va_list args;
	ssize_t ret;

	va_start(args, fmt);
	ret = vfdprintf(fd, fmt, args);
	va_end(args);

	return ret;
}

