/* the Music Player Daemon (MPD)
 * (c)2003-2004 by Warren Dukes (shank@mercury.chem.pitt.edu)
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

#ifndef CONF_H
#define CONF_H

#include "../config.h"
#include "list.h"

#define CONF_PORT			"port"
#define CONF_MUSIC_DIR			"music_directory"
#define CONF_PLAYLIST_DIR		"playlist_directory"
#define CONF_LOG_FILE			"log_file"
#define CONF_ERROR_FILE			"error_file"
#define CONF_CONN_TIMEOUT		"connection_timeout"
#define CONF_MIXER_DEVICE		"mixer_device"
#define CONF_MAX_CONN			"max_connections"
#define CONF_MAX_PLAYLIST_LENGTH	"max_playlist_length"
#define CONF_BUFFER_BEFORE_PLAY		"buffer_before_play"
#define CONF_MAX_COMMAND_LIST_SIZE	"max_command_list_size"
#define CONF_MAX_OUTPUT_BUFFER_SIZE	"max_output_buffer_size"
#define CONF_SAVE_ABSOLUTE_PATHS	"save_absolute_paths_in_playlists"
#define CONF_BIND_TO_ADDRESS		"bind_to_address"
#define CONF_MIXER_TYPE			"mixer_type"
#define CONF_STATE_FILE			"state_file"
#define CONF_USER			"user"
#define CONF_DB_FILE			"db_file"
#define CONF_LOG_LEVEL			"log_level"
#define CONF_MIXER_CONTROL		"mixer_control"
#define CONF_AUDIO_WRITE_SIZE		"audio_write_size"
#define CONF_FS_CHARSET			"filesystem_charset"
#define CONF_PASSWORD			"password"
#define CONF_DEFAULT_PERMS		"default_permissions"
#define CONF_AUDIO_BUFFER_SIZE		"audio_buffer_size"
#define CONF_REPLAYGAIN			"replaygain"
#define CONF_AUDIO_OUTPUT_FORMAT	"audio_output_format"
#define CONF_HTTP_PROXY_HOST		"http_proxy_host"
#define CONF_HTTP_PROXY_PORT		"http_proxy_port"
#define CONF_HTTP_PROXY_USER		"http_proxy_user"
#define CONF_HTTP_PROXY_PASSWORD	"http_proxy_password"
#define CONF_REPLAYGAIN_PREAMP		"replaygain_preamp"
#define CONF_ID3V1_ENCODING		"id3v1_encoding"

/* general audio output options */
#define CONF_AUDIO_OUTPUT		"audio_output"
#define CONF_AO_NAME			"name"
#define CONF_AO_TYPE			"type"
#define CONF_AO_FORMAT			"format"

/* libao options */
#define CONF_AO_DRIVER			"driver"
#define CONF_AO_OPTIONS			"options"

/* shout */
#define CONF_AO_MOUNT			"mount"
#define CONF_AO_QUALITY			"quality"
#define CONF_AO_USER			"user"
#define CONF_AO_PORT			"port"
#define CONF_AO_PASSWORD		"password"

typedef struct _ConfigParam {
	char * name;
	List * subParamsList;
} ConfigParam;

typedef struct _ConfigEntry {
	char * name;
	char * value;
	int line;
	List * subEntriesList;
	int quieried;
} ConfigEntry;

void initConf();

void readConf(char * file);

/* don't free the returned value
   set _last_ to NULL to get first entry */
ConfigEntry * getNextChildConfigEntry(ConfigEntry * parent, char * name, 
		ListNode ** last);

#define getNextConfigEntry(name, last) \
	getNextChildConfigEntry(NULL, name, NULL)

#define getChildConfigEntry(parent, name) \
	getNextChildConfigEntry(parent, name, NULL)

#define getConfigEntry(name) getNextConfigEntry(name, NULL)

char * getChildConfigEntryValue(ConfigEntry * parent, char * name);

#define getConfigEntryValue(name) getChildConfigEntryValue(NULL, name)

char * forceAndGetChildConfigEntryValue(ConfigEntry * parent, char * name);

#define forceAndGetConfigEntryValue(name) \
	forceAndGetChildConfigEntryValue(NULL, name)

ConfigParam * registerChildConfigParam(ConfigParam * parent, char * name);

#define registerConfigParam(name) registerChildConfigParam(NULL, name)

char * parseChildConfigFilePath(ConfigEntry * parent, char * name, int force);

#define parseConfigFilePath(name, force) \
	parseChildConfigFilePath(NULL, name, force)

#endif
