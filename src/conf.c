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

#include "conf.h"

#include "log.h"

#include "utils.h"
#include "buffer2array.h"
#include "list.h"

#include <sys/param.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <pwd.h>

#define MAX_STRING_SIZE	MAXPATHLEN+80

#define CONF_COMMENT		'#'

#define CONF_REPEATABLE_MASK	0x01
#define CONF_BLOCK_MASK		0x02

typedef struct _configEntry {
	unsigned char mask;
	List * configParamList;
} ConfigEntry;

static List * configEntriesList = NULL;

static BlockParam * newBlockParam(char * name, char * value) {
	BlockParam * ret = malloc(sizeof(BlockParam));

	ret->name = strdup(name);
	ret->value = strdup(value);

	return ret;
}

static void freeBlockParam(BlockParam * param) {
	free(param->name);
	free(param->value);
	free(param);
}

static ConfigParam * newConfigParam(char * value) {
	ConfigParam * ret = malloc(sizeof(ConfigParam));

	if(value) ret->value = NULL;

	ret->numberOfBlockParams = 0;
	ret->blockParams = NULL;

	return ret;
}

static void freeConfigParam(ConfigParam * param) {
	int i;

	free(param->value);

	for(i=0; i<param->numberOfBlockParams; i++) {
		freeBlockParam(param->blockParams+i);
	}

	free(param);
}

ConfigEntry * newConfigEntry(int repeatable, int block) {
	ConfigEntry * ret =  malloc(sizeof(ConfigEntry));

	ret->mask = 0;
	ret->configParamList = makeList((ListFreeDataFunc *)freeConfigParam);

	if(repeatable) ret->mask &= CONF_REPEATABLE_MASK;
	if(block) ret->mask &= CONF_BLOCK_MASK;

	return ret;
}

void freeConfigEntry(ConfigEntry * entry) {
	freeList(entry->configParamList);
	free(entry);
}

void registerConfigParam(char * name, int repeatable, int block) {
	ConfigEntry * entry;

	if(findInList(configEntriesList, name, NULL)) {
		ERROR("config parameter \"%s\" already registered\n", name);
		exit(EXIT_FAILURE);
	}

	entry = newConfigEntry(repeatable, block);

	insertInList(configEntriesList, name, entry);
}

void initConf() {
	configEntriesList = makeList((ListFreeDataFunc *)freeConfigEntry);

	registerConfigParam(CONF_PORT, 				0,	0);
	registerConfigParam(CONF_MUSIC_DIR,			0,	0);
	registerConfigParam(CONF_PLAYLIST_DIR,			0,	0);
	registerConfigParam(CONF_LOG_FILE,			0,	0);
	registerConfigParam(CONF_ERROR_FILE,			0,	0);
	registerConfigParam(CONF_CONN_TIMEOUT,			0,	0);
	registerConfigParam(CONF_MIXER_DEVICE,			0,	0);
	registerConfigParam(CONF_MAX_CONN,			0,	0);
	registerConfigParam(CONF_MAX_PLAYLIST_LENGTH,		0,	0);
	registerConfigParam(CONF_BUFFER_BEFORE_PLAY,		0,	0);
	registerConfigParam(CONF_MAX_COMMAND_LIST_SIZE,		0,	0);
	registerConfigParam(CONF_MAX_OUTPUT_BUFFER_SIZE,	0,	0);
	registerConfigParam(CONF_AUDIO_OUTPUT,			1,	1);
	registerConfigParam(CONF_SAVE_ABSOLUTE_PATHS,		0,	0);
	registerConfigParam(CONF_BIND_TO_ADDRESS,		1,	0);
	registerConfigParam(CONF_MIXER_TYPE,			0,	0);
	registerConfigParam(CONF_STATE_FILE,			0,	0);
	registerConfigParam(CONF_USER,				0,	0);
	registerConfigParam(CONF_DB_FILE,			0,	0);
	registerConfigParam(CONF_LOG_LEVEL,			0,	0);
	registerConfigParam(CONF_MIXER_CONTROL,			0,	0);
	registerConfigParam(CONF_AUDIO_WRITE_SIZE,		0,	0);
	registerConfigParam(CONF_FS_CHARSET,			0,	0);
	registerConfigParam(CONF_PASSWORD,			1,	0);
	registerConfigParam(CONF_DEFAULT_PERMS,			0,	0);
	registerConfigParam(CONF_AUDIO_BUFFER_SIZE,		0,	0);
	registerConfigParam(CONF_REPLAYGAIN,			0,	0);
	registerConfigParam(CONF_AUDIO_OUTPUT_FORMAT,		0,	0);
	registerConfigParam(CONF_HTTP_PROXY_HOST,		0,	0);
	registerConfigParam(CONF_HTTP_PROXY_PORT,		0,	0);
	registerConfigParam(CONF_HTTP_PROXY_USER,		0,	0);
	registerConfigParam(CONF_HTTP_PROXY_PASSWORD,		0,	0);
	registerConfigParam(CONF_REPLAYGAIN_PREAMP,		0,	0);
	registerConfigParam(CONF_ID3V1_ENCODING,		0,	0);
}

int readConf(char * file) {
	char * conf_strings[CONF_NUMBER_OF_PARAMS] = {
		"port",
		"music_directory",
		"playlist_directory",
		"log_file",
		"error_file",
		"connection_timeout",
		"mixer_device",
		"max_connections",
		"max_playlist_length",
		"buffer_before_play",
		"max_command_list_size",
		"max_output_buffer_size",
		"ao_driver",
		"ao_driver_options",
		"save_absolute_paths_in_playlists",
		"bind_to_address",
		"mixer_type",
		"state_file",
		"user",
		"db_file",
		"log_level",
		"mixer_control",
		"audio_write_size",
		"filesystem_charset",
		"password",
		"default_permissions",
		"audio_buffer_size",
                "replaygain",
                "audio_output_format",
                "http_proxy_host",
                "http_proxy_port",
		"http_proxy_user",
		"http_proxy_password",
		"replaygain_preamp",
		"shout_host",
		"shout_port",
		"shout_password",
		"shout_mount",
		"shout_name",
		"shout_user",
		"shout_quality",
		"id3v1_encoding",
		"shout_format"
	};

	int conf_absolutePaths[CONF_NUMBER_OF_PATHS] = {
		CONF_MUSIC_DIRECTORY,
		CONF_PLAYLIST_DIRECTORY,
		CONF_LOG_FILE,
		CONF_ERROR_FILE,
		CONF_STATE_FILE,
		CONF_DB_FILE
	};

	int conf_required[CONF_NUMBER_OF_REQUIRED] = {
		CONF_MUSIC_DIRECTORY,
		CONF_PLAYLIST_DIRECTORY,
		CONF_LOG_FILE,
		CONF_ERROR_FILE,
		CONF_PORT
	};

	short conf_allowCat[CONF_NUMBER_OF_ALLOW_CATS] = {
		CONF_PASSWORD
	};

	FILE * fp;
	char string[MAX_STRING_SIZE+1];
	char ** array;
	int i;
	int numberOfArgs;
	short allowCat[CONF_NUMBER_OF_PARAMS];
	int count = 0;

	for(i=0;i<CONF_NUMBER_OF_PARAMS;i++) allowCat[i] = 0;

	for(i=0;i<CONF_NUMBER_OF_ALLOW_CATS;i++) allowCat[conf_allowCat[i]] = 1;

	if(!(fp=fopen(file,"r"))) {
		ERROR("problems opening file %s for reading\n",file);
		exit(EXIT_FAILURE);
	}

	while(myFgets(string,sizeof(string),fp)) {
		count++;

		if(string[0]==CONF_COMMENT) continue;
		numberOfArgs = buffer2array(string,&array);
		if(numberOfArgs==0) continue;
		if(2!=numberOfArgs) {
			ERROR("improperly formated config file at line %i: %s\n",count,string);
			exit(EXIT_FAILURE);
		}
		i = 0;
		while(i<CONF_NUMBER_OF_PARAMS && 0!=strcmp(conf_strings[i],array[0])) i++;
		if(i>=CONF_NUMBER_OF_PARAMS) {
			ERROR("unrecognized paramater in conf at line %i: %s\n",count,string);
			exit(EXIT_FAILURE);
		}
		
		if(conf_params[i]!=NULL) {
			if(allowCat[i]) {
				conf_params[i] = realloc(conf_params[i],
						strlen(conf_params[i])+
						strlen(CONF_CAT_CHAR)+
						strlen(array[1])+1);
				strcat(conf_params[i],CONF_CAT_CHAR);
				strcat(conf_params[i],array[1]);
			}
			else {
				free(conf_params[i]);
				conf_params[i] = strdup(array[1]);
			}
		}
		else conf_params[i] = strdup(array[1]);
		free(array[0]);
		free(array[1]);
		free(array);
	}

	fclose(fp);

	for(i=0;i<CONF_NUMBER_OF_REQUIRED;i++) {
		if(conf_params[conf_required[i]] == NULL) {
			ERROR("%s is unassigned in conf file\n",
					conf_strings[conf_required[i]]);
			exit(EXIT_FAILURE);
		}
	}

	for(i=0;i<CONF_NUMBER_OF_PATHS;i++) {
		if(conf_params[conf_absolutePaths[i]] && 
			conf_params[conf_absolutePaths[i]][0]!='/' &&
			conf_params[conf_absolutePaths[i]][0]!='~') 
		{
			ERROR("\"%s\" is not an absolute path\n",
					conf_params[conf_absolutePaths[i]]);
			exit(EXIT_FAILURE);
		}
		/* Parse ~ in path */
		else if(conf_params[conf_absolutePaths[i]] &&
			conf_params[conf_absolutePaths[i]][0]=='~') 
		{
			struct passwd * pwd = NULL;
			char * path;
			int pos = 1;
			if(conf_params[conf_absolutePaths[i]][1]=='/' ||
				conf_params[conf_absolutePaths[i]][1]=='\0') 
			{
				if(conf_params[CONF_USER] && 
						strlen(conf_params[CONF_USER]))
				{
					pwd = getpwnam(
						conf_params[CONF_USER]);
					if(!pwd) {
						ERROR("no such user: %s\n",
							conf_params[CONF_USER]);
						exit(EXIT_FAILURE);
					}
				}
				else {
					uid_t uid = geteuid();
					if((pwd = getpwuid(uid)) == NULL) {
						ERROR("problems getting passwd "
							"entry "
							"for current user\n");
						exit(EXIT_FAILURE);
					}
				}
			}
			else {
				int foundSlash = 0;
				char * ch = &(
					conf_params[conf_absolutePaths[i]][1]);
				for(;*ch!='\0' && *ch!='/';ch++);
				if(*ch=='/') foundSlash = 1;
				* ch = '\0';
				pos+= ch-
					&(conf_params[
					conf_absolutePaths[i]][1]);
				if((pwd = getpwnam(&(conf_params[
					conf_absolutePaths[i]][1]))) == NULL) 
				{
					ERROR("user \"%s\" not found\n",
						&(conf_params[
						conf_absolutePaths[i]][1]));
					exit(EXIT_FAILURE);
				}
				if(foundSlash) *ch = '/';
			}
			path = malloc(strlen(pwd->pw_dir)+strlen(
				&(conf_params[conf_absolutePaths[i]][pos]))+1);
			strcpy(path,pwd->pw_dir);
			strcat(path,&(conf_params[conf_absolutePaths[i]][pos]));
			free(conf_params[conf_absolutePaths[i]]);
			conf_params[conf_absolutePaths[i]] = path;
		}
	}

	return conf_params;
}

char ** getConf() {
	return conf_params;
}
