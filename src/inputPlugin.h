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

#ifndef INPUT_PLUGIN_H
#define INPUT_PLUGIN_H

#include "inputStream.h"
#include "outputBuffer.h"
#include "metadata_pipe.h"
#include "decode.h"
#include "tag.h"
#include "tag_id3.h"

/* valid values for streamTypes in the InputPlugin struct: */
#define INPUT_PLUGIN_STREAM_FILE	0x01
#define INPUT_PLUGIN_STREAM_URL		0x02

/* optional, set this to NULL if the InputPlugin doesn't have/need one
 * this must return < 0 if there is an error and >= 0 otherwise */
typedef int (*InputPlugin_initFunc) (void);

/* optional, set this to NULL if the InputPlugin doesn't have/need one */
typedef void (*InputPlugin_finishFunc) (void);

/* boolean return value, returns 1 if the InputStream is decodable by
 * the InputPlugin, 0 if not */
typedef unsigned int (*InputPlugin_tryDecodeFunc) (InputStream *);

/* this will be used to decode InputStreams, and is recommended for files
 * and networked (HTTP) connections.
 *
 * returns -1 on error, 0 on success */
typedef int (*InputPlugin_streamDecodeFunc) (InputStream *);

/* use this if and only if your InputPlugin can only be passed a filename or
 * handle as input, and will not allow callbacks to be set (like Ogg-Vorbis
 * and FLAC libraries allow)
 *
 * returns -1 on error, 0 on success */
typedef int (*InputPlugin_fileDecodeFunc) (char *path);

/* file should be the full path!  Returns NULL if a tag cannot be found
 * or read */
typedef struct mpd_tag *(*InputPlugin_tagDupFunc) (char *file);

typedef struct _InputPlugin {
	const char *name;
	InputPlugin_initFunc initFunc;
	InputPlugin_finishFunc finishFunc;
	InputPlugin_tryDecodeFunc tryDecodeFunc;
	InputPlugin_streamDecodeFunc streamDecodeFunc;
	InputPlugin_fileDecodeFunc fileDecodeFunc;
	InputPlugin_tagDupFunc tagDupFunc;

	/* one or more of the INPUT_PLUGIN_STREAM_* values OR'd together */
	unsigned char streamTypes;

	/* last element in these arrays must always be a NULL: */
	const char *const*suffixes;
	const char *const*mimeTypes;
} InputPlugin;

/* individual functions to load/unload plugins */
void loadInputPlugin(InputPlugin * inputPlugin);
void unloadInputPlugin(InputPlugin * inputPlugin);

/* interface for using plugins */

InputPlugin *getInputPluginFromSuffix(const char *suffix, unsigned int next);

InputPlugin *getInputPluginFromMimeType(const char *mimeType, unsigned int next);

InputPlugin *getInputPluginFromName(const char *name);

void printAllInputPluginSuffixes(FILE * fp);

/* this is where we "load" all the "plugins" ;-) */
void initInputPlugins(void);

/* this is where we "unload" all the "plugins" */
void finishInputPlugins(void);

extern InputPlugin mp3Plugin;
extern InputPlugin oggvorbisPlugin;
extern InputPlugin flacPlugin;
extern InputPlugin oggflacPlugin;
extern InputPlugin audiofilePlugin;
extern InputPlugin mp4Plugin;
extern InputPlugin aacPlugin;
extern InputPlugin mpcPlugin;
extern InputPlugin wavpackPlugin;
extern InputPlugin modPlugin;

#endif
