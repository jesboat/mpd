/*
 * Copyright (C) 2003-2013 The Music Player Daemon Project
 * http://www.musicpd.org
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
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#ifndef MPD_PATH_HXX
#define MPD_PATH_HXX

#include "check.h"
#include "gcc.h"

#include <glib.h>

#include <algorithm>

#include <assert.h>
#include <string.h>
#include <limits.h>

#if !defined(MPD_PATH_MAX)
#  if defined(MAXPATHLEN)
#    define MPD_PATH_MAX MAXPATHLEN
#  elif defined(PATH_MAX)
#    define MPD_PATH_MAX PATH_MAX
#  else
#    define MPD_PATH_MAX 256
#  endif
#endif

void path_global_init();

void path_global_finish();

/**
 * Converts a file name in the filesystem charset to UTF-8.  Returns
 * NULL on failure.
 */
char *
fs_charset_to_utf8(const char *path_fs);

/**
 * Converts a file name in UTF-8 to the filesystem charset.  Returns a
 * duplicate of the UTF-8 string on failure.
 */
char *
utf8_to_fs_charset(const char *path_utf8);

const char *path_get_fs_charset();

/**
 * A path name in the native file system character set.
 */
class Path {
	char *value;

	struct Donate {};

	Path(Donate, char *_value):value(_value) {}

public:
	Path(Path &&other):value(other.value) {
		other.value = nullptr;
	}

	Path(const Path &other) = delete;

#if 0
	/* this is the correct implementation, but unfortunately it
	   disables compiler optimizations */
	Path(const Path &other)
		:value(g_strdup(other.value)) {}
#endif


	~Path() {
		/* free() can be optimized by gcc, while g_free() can
		   not: when the compiler knows that the value is
		   nullptr, it will not emit a free() call in the
		   inlined destructor; however on Windows, we need to
		   call g_free(), because the value has been allocated
		   by GLib, and on Windows, this matters */
#ifdef WIN32
		g_free(value);
#else
		free(value);
#endif
	}

	gcc_const
	static Path Null() {
		return Path(Donate(), nullptr);
	}

	gcc_pure
	static Path Build(const char *a, const char *b) {
		return Path(Donate(), g_build_filename(a, b, nullptr));
	}

	static Path Build(const char *a, const Path &b) {
		return Build(a, b.c_str());
	}

	static Path Build(const Path &a, const Path &b) {
		return Build(a.c_str(), b.c_str());
	}

	gcc_pure
	static Path FromFS(const char *fs) {
		return Path(Donate(), g_strdup(fs));
	}

	gcc_pure
	static Path FromUTF8(const char *utf8) {
		return Path(Donate(), utf8_to_fs_charset(utf8));
	}

	Path &operator=(const Path &other) {
		value = g_strdup(other.value);
		return *this;
	}

	Path &operator=(Path &&other) {
		std::swap(value, other.value);
		return *this;
	}

	char *Steal() {
		char *result = value;
		value = nullptr;
		return result;
	}

	bool IsNull() const {
		return value == nullptr;
	}

	void SetNull() {
		g_free(value);
		value = nullptr;
	}

	gcc_pure
	size_t length() const {
		assert(value != nullptr);

		return strlen(value);
	}

	gcc_pure
	const char *c_str() const {
		assert(value != nullptr);

		return value;
	}

	/**
	 * Convert the path to UTF-8.  The caller is responsible for
	 * freeing the return value with g_free().  Returns nullptr on
	 * error.
	 */
	char *ToUTF8() const {
		return value != nullptr
			? fs_charset_to_utf8(value)
			: nullptr;
	}
};

#endif
