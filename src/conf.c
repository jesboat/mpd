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
#define CONF_BLOCK_BEGIN	"{"
#define CONF_BLOCK_END		"}"

static ConfigParam * globalParam = NULL;
static ConfigEntry * globalEntry = NULL;

static void freeConfigParam(ConfigParam * param) {
	if(param->name) free(param->name);
	freeList(param->subParamsList);

	free(param);
}

static ConfigParam * newConfigParam(char * name) {
	ConfigParam * ret = malloc(sizeof(ConfigParam));

	ret->subParamsList = makeList((ListFreeDataFunc *)freeConfigParam);
	if(name) ret->name = strdup(name);
	else name = NULL;

	return ret;
}

void freeConfigEntry(ConfigEntry * entry) {
	freeList(entry->subEntriesList);
	if(entry->name) free(entry->name);
	if(entry->value) free(entry->value);
	free(entry);
}

ConfigEntry * newConfigEntry(char * name, char * value, int line) {
	ConfigEntry * ret =  malloc(sizeof(ConfigEntry));

	ret->subEntriesList = makeList((ListFreeDataFunc *)freeConfigEntry);

	if(name) ret->name = strdup(name);
	else ret->name = NULL;

	if(value) ret->value = strdup(value);
	else ret->value = NULL;

	ret->line = line;

	ret->quieried = 0;

	return ret;
}

ConfigParam * registerChildConfigParam(ConfigParam * parent, char * name) {
	ConfigParam * param;

	if(parent == NULL) parent = globalParam;

	if(!name) {
		ERROR("no name for new config parameter given!\n");
		exit(EXIT_FAILURE);
	}

	if(findInList(parent->subParamsList, name, NULL)) {
		ERROR("config parameter \"%s\" already registered\n", name);
		exit(EXIT_FAILURE);
	}

	param = newConfigParam(name);

	insertInList(parent->subParamsList, name, param);

	return param;
}

void initConf() {
	ConfigParam * parent;
	globalParam = newConfigParam(NULL);

	registerConfigParam(CONF_PORT);
	registerConfigParam(CONF_MUSIC_DIR);
	registerConfigParam(CONF_PLAYLIST_DIR);
	registerConfigParam(CONF_LOG_FILE);
	registerConfigParam(CONF_ERROR_FILE);
	registerConfigParam(CONF_CONN_TIMEOUT);
	registerConfigParam(CONF_MIXER_DEVICE);
	registerConfigParam(CONF_MAX_CONN);
	registerConfigParam(CONF_MAX_PLAYLIST_LENGTH);
	registerConfigParam(CONF_BUFFER_BEFORE_PLAY);
	registerConfigParam(CONF_MAX_COMMAND_LIST_SIZE);
	registerConfigParam(CONF_MAX_OUTPUT_BUFFER_SIZE);
	registerConfigParam(CONF_SAVE_ABSOLUTE_PATHS);
	registerConfigParam(CONF_BIND_TO_ADDRESS);
	registerConfigParam(CONF_MIXER_TYPE);
	registerConfigParam(CONF_STATE_FILE);
	registerConfigParam(CONF_USER);
	registerConfigParam(CONF_DB_FILE);
	registerConfigParam(CONF_LOG_LEVEL);
	registerConfigParam(CONF_MIXER_CONTROL);
	registerConfigParam(CONF_AUDIO_WRITE_SIZE);
	registerConfigParam(CONF_FS_CHARSET);
	registerConfigParam(CONF_PASSWORD);
	registerConfigParam(CONF_DEFAULT_PERMS);
	registerConfigParam(CONF_AUDIO_BUFFER_SIZE);
	registerConfigParam(CONF_REPLAYGAIN);
	registerConfigParam(CONF_AUDIO_OUTPUT_FORMAT);
	registerConfigParam(CONF_HTTP_PROXY_HOST);
	registerConfigParam(CONF_HTTP_PROXY_PORT);
	registerConfigParam(CONF_HTTP_PROXY_USER);
	registerConfigParam(CONF_HTTP_PROXY_PASSWORD);
	registerConfigParam(CONF_REPLAYGAIN_PREAMP);
	registerConfigParam(CONF_ID3V1_ENCODING);

	/* register audio output parameters */
	parent = registerConfigParam(CONF_AUDIO_OUTPUT);

	/*  general */
	registerChildConfigParam(parent, CONF_AO_NAME);
	registerChildConfigParam(parent, CONF_AO_TYPE);
	registerChildConfigParam(parent, CONF_AO_FORMAT);

	/* ao */
	registerChildConfigParam(parent, CONF_AO_DRIVER);
	registerChildConfigParam(parent, CONF_AO_OPTIONS);

	/* shout */
	registerChildConfigParam(parent, CONF_AO_MOUNT);
	registerChildConfigParam(parent, CONF_AO_QUALITY);
	registerChildConfigParam(parent, CONF_AO_USER);
	registerChildConfigParam(parent, CONF_AO_PORT);
	registerChildConfigParam(parent, CONF_AO_PASSWORD);
}

static ConfigEntry * readConfigBlock(FILE * fp, int * count, char * string,
		ConfigParam * parentParam) {
	ConfigEntry * parentEntry;
	ConfigEntry * entry;
	ConfigParam * param;
	char ** array;
	void * voidPtr;
	int i;
	int numberOfArgs;
	int argsMinusComment;

	parentEntry = newConfigEntry(parentParam->name, NULL, *count);

	while(myFgets(string, MAX_STRING_SIZE ,fp)) {
		(*count)++;

		numberOfArgs = buffer2array(string, &array);

		for(i=0; i<numberOfArgs; i++) {
			if(array[i][0] == CONF_COMMENT) break;
		}

		argsMinusComment = i;

		if(0 == argsMinusComment) continue;

		if(1 == argsMinusComment && 
				0 == strcmp(array[0], CONF_BLOCK_END))
		{
			break;
		}

		if(2 != argsMinusComment) {
			ERROR("improperly formated config file at line %i:"
					" %s\n", *count, string);
			exit(EXIT_FAILURE);
		}

		if(!findInList(parentParam->subParamsList, array[0], &voidPtr))
		{
			ERROR("unrecognized paramater in config file at line "
					"%i: %s\n", *count, string);
			exit(EXIT_FAILURE);
		}

		param = (ConfigParam *) voidPtr;

		if(0 == strcmp(array[1], CONF_BLOCK_BEGIN)) {
			entry = readConfigBlock(fp, count, string, param);
		}
		else entry = newConfigEntry(array[0], array[1], *count);

		insertInList(parentEntry->subEntriesList, array[0], entry);

		freeArgArray(array, numberOfArgs);
	}

	return parentEntry;
}

void readConf(char * file) {
	FILE * fp;
	char string[MAX_STRING_SIZE+1];
	int count = 0;

	if(!(fp=fopen(file,"r"))) {
		ERROR("problems opening file %s for reading\n",file);
		exit(EXIT_FAILURE);
	}

	globalEntry = readConfigBlock(fp, &count, string, globalParam);

	/* if feof: print error */

	fclose(fp);
}

ConfigEntry * getNextChildConfigEntry(ConfigEntry * parentEntry, char * name, 
		ListNode ** last) 
{
	ListNode * node;

	if(parentEntry == NULL) parentEntry = globalEntry;

	if(!last || !(*last)) node = parentEntry->subEntriesList->firstNode;
	else node = *last;

	while(node != NULL && 0 != strcmp(node->key, name)) {
		node = node->nextNode;
	}

	if(!node) return NULL;

	if(last) *last = node; 

	return node->data;
}

char * getChildConfigEntryValue(ConfigEntry * parentEntry, char * name) {
	ConfigEntry * entry = getConfigEntry(name);

	if(!entry) return NULL;

	return entry->value;
}

char * forceAndGetChildConfigEntryValue(ConfigEntry * parentEntry, char * name) 
{
	char * value = getChildConfigEntryValue(parentEntry, name);

	if(!value) {
		if(parentEntry) {
			ERROR("parsing \%s\" (line %i): ", parentEntry->name, 
				parentEntry->line);
		}
		ERROR("\"%s\" not found in config file\n", name);
		exit(EXIT_FAILURE);
	}

	return value;
}

char * parseChildConfigFilePath(ConfigEntry * parentEntry, char * name, 
		int force) 
{
	ConfigEntry * entry = getChildConfigEntry(parentEntry, name);
	char * path;

	if(!entry && force) {
		if(parentEntry) {
			ERROR("parsing \%s\" (line %i): ", parentEntry->name, 
				parentEntry->line);
		}
		ERROR("config parameter \"%s\" not found\n", name);
		exit(EXIT_FAILURE);
	}

	if(!entry) return NULL;

	path = entry->value;

	if(path[0] != '/' && path[0] != '~') {
		ERROR("\"%s\" is not an absolute path at line %i\n",
				entry->value, entry->line);
		exit(EXIT_FAILURE);
	}
	// Parse ~ in path 
	else if(path[0] == '~') {
		struct passwd * pwd = NULL;
		char * newPath;
		int pos = 1;
		if(path[1]=='/' || path[1] == '\0') {
			ConfigEntry * userEntry = getConfigEntry(CONF_USER);

			if(userEntry) {
				pwd = getpwnam(userEntry->value);
				if(!pwd) {
					ERROR("no such user %s at line %i\n",
							userEntry->value, 
							userEntry->line);
					exit(EXIT_FAILURE);
				}
			}
			else {
				uid_t uid = geteuid();
				if((pwd = getpwuid(uid)) == NULL) {
					ERROR("problems getting passwd entry "
							"for current user\n");
					exit(EXIT_FAILURE);
				}
			}
		}
		else {
			int foundSlash = 0;
			char * ch = path+1;
			for(;*ch!='\0' && *ch!='/';ch++);
			if(*ch=='/') foundSlash = 1;
			* ch = '\0';
			pos+= ch-path+1;
			if((pwd = getpwnam(path+1)) == NULL) {
				ERROR("user \"%s\" not found at line %i\n",
						path+1, entry->line);
				exit(EXIT_FAILURE);
			}
			if(foundSlash) *ch = '/';
		}
		newPath = malloc(strlen(pwd->pw_dir)+strlen(path+pos)+1);
		strcpy(newPath, pwd->pw_dir);
		strcat(newPath, path+pos);
		free(entry->value);
		entry->value = newPath;
	}

	return entry->value;
}
