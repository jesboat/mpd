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

#ifndef UTILS_H
#define UTILS_H

#include "gcc.h"
#include "os_compat.h"

#define ARRAY_SIZE(x) (sizeof(x)/sizeof(x[0]))

char *myFgets(char *buffer, int bufferSize, FILE * fp);

char *string_toupper(char *str);

char *strDupToUpper(char *str); /* avoid, use string_toupper instead */

void stripReturnChar(char *string);

void my_usleep(long usec);

int ipv6Supported(void);

char *appendToString(char *dest, const char *src);

unsigned long readLEuint32(const unsigned char *p);

/* trivial functions, keep them inlined */
static inline void xclose(int fd)
{
	while (close(fd) && errno == EINTR);
}

static inline ssize_t xread(int fd, void *buf, size_t len)
{
	ssize_t nr;
	while (1) {
		nr = read(fd, buf, len);
		if ((nr < 0) && (errno == EAGAIN || errno == EINTR))
			continue;
		return nr;
	}
}

static inline ssize_t xwrite(int fd, const void *buf, size_t len)
{
	ssize_t nr;
	while (1) {
		nr = write(fd, buf, len);
		if ((nr < 0) && (errno == EAGAIN || errno == EINTR))
			continue;
		return nr;
	}
}

mpd_malloc char *xstrdup(const char *s);

mpd_malloc void *xmalloc(size_t size);

mpd_malloc void *xrealloc(void *ptr, size_t size);

mpd_malloc void *xcalloc(size_t nmemb, size_t size);

char *parsePath(char *path);

int set_nonblocking(int fd);

void init_async_pipe(int file_des[2]);

void xpthread_mutex_init(pthread_mutex_t *m, const pthread_mutexattr_t *a);

void xpthread_cond_init(pthread_cond_t *c, pthread_condattr_t *a);

void xpthread_mutex_destroy(pthread_mutex_t *mutex);

void xpthread_cond_destroy(pthread_cond_t *cond);

/*
 * Work-arounds for braindead APIs that require non-const pointers:
 *   ao_play(), free(), vorbis_comment_add_tag(), iconv()
 */
static inline void * deconst_ptr(const void *ptr)
{
	union { const void *in; void *out; } u;
	u.in = ptr;
	return u.out;
}

#endif
