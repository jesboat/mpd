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

#include "directory.h"

#include "command.h"
#include "conf.h"
#include "interface.h"
#include "listen.h"
#include "log.h"
#include "ls.h"
#include "path.h"
#include "playlist.h"
#include "sig_handlers.h"
#include "stats.h"
#include "utils.h"
#include "volume.h"
#include "ack.h"
#include "myfprintf.h"
#include "dbUtils.h"

#define DIRECTORY_DIR		"directory: "
#define DIRECTORY_MTIME		"mtime: "
#define DIRECTORY_BEGIN		"begin: "
#define DIRECTORY_END		"end: "
#define DIRECTORY_INFO_BEGIN	"info_begin"
#define DIRECTORY_INFO_END	"info_end"
#define DIRECTORY_MPD_VERSION	"mpd_version: "
#define DIRECTORY_FS_CHARSET	"fs_charset: "

#define DIRECTORY_UPDATE_EXIT_NOUPDATE  0
#define DIRECTORY_UPDATE_EXIT_UPDATE    1
#define DIRECTORY_UPDATE_EXIT_ERROR     2

#define DIRECTORY_RETURN_NOUPDATE       0
#define DIRECTORY_RETURN_UPDATE         1
#define DIRECTORY_RETURN_ERROR         -1

static Directory *mp3rootDirectory;

static time_t directory_dbModTime;

static volatile int directory_updatePid;

static volatile int directory_reReadDB;

static volatile mpd_uint16 directory_updateJobId;

static DirectoryList *newDirectoryList(void);

static int addToDirectory(Directory * directory,
			  const char *shortname, const char *name);

static void freeDirectoryList(DirectoryList * list);

static void freeDirectory(Directory * directory);

static int exploreDirectory(Directory * directory);

static int updateDirectory(Directory * directory);

static void deleteEmptyDirectoriesInDirectory(Directory * directory);

static void removeSongFromDirectory(Directory * directory,
				    const char *shortname);

static int addSubDirectoryToDirectory(Directory * directory,
				      const char *shortname,
				      const char *name, struct stat *st);

static Directory *getDirectoryDetails(const char *name,
				      const char **shortname);

static Directory *getDirectory(const char *name);

static Song *getSongDetails(const char *file, const char **shortnameRet,
			    Directory ** directoryRet);

static int updatePath(const char *utf8path);

static void sortDirectory(Directory * directory);

static int inodeFoundInParent(Directory * parent, ino_t inode, dev_t device);

static int statDirectory(Directory * dir);

static char *getDbFile(void)
{
	ConfigParam *param = parseConfigFilePath(CONF_DB_FILE, 1);

	assert(param);
	assert(param->value);

	return param->value;
}

static void clearUpdatePid(void)
{
	directory_updatePid = 0;
}

int isUpdatingDB(void)
{
	if (directory_updatePid > 0 || directory_reReadDB) {
		return directory_updateJobId;
	}
	return 0;
}

void directory_sigChldHandler(int pid, int status)
{
	if (directory_updatePid == pid) {
		if (WIFSIGNALED(status) && WTERMSIG(status) != SIGTERM) {
			/* ERROR("update process died from a "
			      "non-TERM signal: %i\n", WTERMSIG(status)); */
		} else if (!WIFSIGNALED(status)) {
			switch (WEXITSTATUS(status)) {
			case DIRECTORY_UPDATE_EXIT_UPDATE:
				directory_reReadDB = 1;
				/* DEBUG("directory_sigChldHandler: "
				      "updated db\n"); */
			case DIRECTORY_UPDATE_EXIT_NOUPDATE:
				/* DEBUG("directory_sigChldHandler: "
				      "update exited succesffully\n"); */
				break;
			/*
			default:
				ERROR("error updating db\n");
			*/
			}
		}
		clearUpdatePid();
	}
}

void readDirectoryDBIfUpdateIsFinished(void)
{
	if (directory_reReadDB && 0 == directory_updatePid) {
		DEBUG("readDirectoryDB since update finished successfully\n");
		readDirectoryDB();
		playlistVersionChange();
		directory_reReadDB = 0;
	}
}

int updateInit(int fd, List * pathList)
{
	if (directory_updatePid > 0) {
		commandError(fd, ACK_ERROR_UPDATE_ALREADY, "already updating");
		return -1;
	}

	/* need to block CHLD signal, cause it can exit before we
	   even get a chance to assign directory_updatePID */
	blockSignals();
	directory_updatePid = fork();
	if (directory_updatePid == 0) {
		/* child */
		int dbUpdated = 0;

		unblockSignals();

		finishSigHandlers();
		closeAllListenSockets();
		freeAllInterfaces();
		finishPlaylist();
		finishVolume();

		if (pathList) {
			ListNode *node = pathList->firstNode;

			while (node) {
				switch (updatePath(node->key)) {
				case 1:
					dbUpdated = 1;
					break;
				case 0:
					break;
				default:
					exit(DIRECTORY_UPDATE_EXIT_ERROR);
				}
				node = node->nextNode;
			}
		} else {
			if ((dbUpdated = updateDirectory(mp3rootDirectory)) < 0) {
				exit(DIRECTORY_UPDATE_EXIT_ERROR);
			}
		}

		if (!dbUpdated)
			exit(DIRECTORY_UPDATE_EXIT_NOUPDATE);

		/* ignore signals since we don't want them to corrupt the db */
		ignoreSignals();
		if (writeDirectoryDB() < 0) {
			exit(DIRECTORY_UPDATE_EXIT_ERROR);
		}
		exit(DIRECTORY_UPDATE_EXIT_UPDATE);
	} else if (directory_updatePid < 0) {
		unblockSignals();
		ERROR("updateInit: Problems forking()'ing\n");
		commandError(fd, ACK_ERROR_SYSTEM,
			     "problems trying to update");
		directory_updatePid = 0;
		return -1;
	}
	unblockSignals();

	directory_updateJobId++;
	if (directory_updateJobId > 1 << 15)
		directory_updateJobId = 1;
	DEBUG("updateInit: fork()'d update child for update job id %i\n",
	      (int)directory_updateJobId);
	fdprintf(fd, "updating_db: %i\n", (int)directory_updateJobId);

	return 0;
}

static DirectoryStat *newDirectoryStat(struct stat *st)
{
	DirectoryStat *ret = xmalloc(sizeof(DirectoryStat));
	ret->inode = st->st_ino;
	ret->device = st->st_dev;
	return ret;
}

static void freeDirectoryStatFromDirectory(Directory * dir)
{
	if (dir->stat)
		free(dir->stat);
	dir->stat = NULL;
}

static DirectoryList *newDirectoryList(void)
{
	return makeList((ListFreeDataFunc *) freeDirectory, 1);
}

static Directory *newDirectory(const char *dirname, Directory * parent)
{
	Directory *directory;

	directory = xmalloc(sizeof(Directory));

	if (dirname && strlen(dirname))
		directory->path = xstrdup(dirname);
	else
		directory->path = NULL;
	directory->subDirectories = newDirectoryList();
	directory->songs = newSongList();
	directory->stat = NULL;
	directory->parent = parent;

	return directory;
}

static void freeDirectory(Directory * directory)
{
	freeDirectoryList(directory->subDirectories);
	freeSongList(directory->songs);
	if (directory->path)
		free(directory->path);
	freeDirectoryStatFromDirectory(directory);
	free(directory);
	/* this resets last dir returned */
	/*getDirectoryPath(NULL); */
}

static void freeDirectoryList(DirectoryList * directoryList)
{
	freeList(directoryList);
}

static void removeSongFromDirectory(Directory * directory, const char *shortname)
{
	void *song;

	if (findInList(directory->songs, shortname, &song)) {
		char path_max_tmp[MPD_PATH_MAX]; /* wasteful */
		LOG("removing: %s\n",
		    get_song_url(path_max_tmp, (Song *) song));
		deleteFromList(directory->songs, shortname);
	}
}

static void deleteEmptyDirectoriesInDirectory(Directory * directory)
{
	ListNode *node = directory->subDirectories->firstNode;
	ListNode *nextNode;
	Directory *subDir;

	while (node) {
		subDir = (Directory *) node->data;
		deleteEmptyDirectoriesInDirectory(subDir);
		nextNode = node->nextNode;
		if (subDir->subDirectories->numberOfNodes == 0 &&
		    subDir->songs->numberOfNodes == 0) {
			deleteNodeFromList(directory->subDirectories, node);
		}
		node = nextNode;
	}
}

/* return values:
   -1 -> error
    0 -> no error, but nothing updated
    1 -> no error, and stuff updated
 */
static int updateInDirectory(Directory * directory,
			     const char *shortname, const char *name)
{
	void *song;
	void *subDir;
	struct stat st;

	if (myStat(name, &st))
		return -1;

	if (S_ISREG(st.st_mode) && hasMusicSuffix(name, 0)) {
		if (0 == findInList(directory->songs, shortname, &song)) {
			addToDirectory(directory, shortname, name);
			return DIRECTORY_RETURN_UPDATE;
		} else if (st.st_mtime != ((Song *) song)->mtime) {
			LOG("updating %s\n", name);
			if (updateSongInfo((Song *) song) < 0) {
				removeSongFromDirectory(directory, shortname);
			}
			return 1;
		}
	} else if (S_ISDIR(st.st_mode)) {
		if (findInList
		    (directory->subDirectories, shortname, (void **)&subDir)) {
			freeDirectoryStatFromDirectory(subDir);
			((Directory *) subDir)->stat = newDirectoryStat(&st);
			return updateDirectory((Directory *) subDir);
		} else {
			return addSubDirectoryToDirectory(directory, shortname,
							  name, &st);
		}
	}

	return 0;
}

/* we don't look at hidden files nor files with newlines in them */
static int skip_path(const char *path)
{
	return (path[0] == '.' || strchr(path, '\n')) ? 1 : 0;
}

/* return values:
   -1 -> error
    0 -> no error, but nothing removed
    1 -> no error, and stuff removed
 */
static int removeDeletedFromDirectory(char *path_max_tmp, Directory * directory)
{
	const char *dirname = (directory && directory->path) ?
	    directory->path : NULL;
	ListNode *node, *tmpNode;
	DirectoryList *subdirs = directory->subDirectories;
	int ret = 0;

	node = subdirs->firstNode;
	while (node) {
		tmpNode = node->nextNode;
		if (node->key) {
			if (dirname)
				sprintf(path_max_tmp, "%s/%s", dirname,
					node->key);
			else
				strcpy(path_max_tmp, node->key);

			if (!isDir(path_max_tmp)) {
				LOG("removing directory: %s\n", path_max_tmp);
				deleteFromList(subdirs, node->key);
				ret = 1;
			}
		}
		node = tmpNode;
	}

	node = directory->songs->firstNode;
	while (node) {
		tmpNode = node->nextNode;
		if (node->key) {
			if (dirname)
				sprintf(path_max_tmp, "%s/%s", dirname,
					node->key);
			else
				strcpy(path_max_tmp, node->key);

			if (!isFile(path_max_tmp, NULL)) {
				removeSongFromDirectory(directory, node->key);
				ret = 1;
			}
		}
		node = tmpNode;
	}

	return ret;
}

static Directory *addDirectoryPathToDB(const char *utf8path,
				       const char **shortname)
{
	char path_max_tmp[MPD_PATH_MAX];
	char *parent;
	Directory *parentDirectory;
	void *directory;

	parent = parent_path(path_max_tmp, utf8path);

	if (strlen(parent) == 0)
		parentDirectory = (void *)mp3rootDirectory;
	else
		parentDirectory = addDirectoryPathToDB(parent, shortname);

	if (!parentDirectory)
		return NULL;

	*shortname = utf8path + strlen(parent);
	while (*(*shortname) && *(*shortname) == '/')
		(*shortname)++;

	if (!findInList
	    (parentDirectory->subDirectories, *shortname, &directory)) {
		struct stat st;
		if (myStat(utf8path, &st) < 0 ||
		    inodeFoundInParent(parentDirectory, st.st_ino, st.st_dev))
			return NULL;
		else {
			directory = newDirectory(utf8path, parentDirectory);
			insertInList(parentDirectory->subDirectories,
				     *shortname, directory);
		}
	}

	/* if we're adding directory paths, make sure to delete filenames
	   with potentially the same name */
	removeSongFromDirectory(parentDirectory, *shortname);

	return (Directory *) directory;
}

static Directory *addParentPathToDB(const char *utf8path, const char **shortname)
{
	char *parent;
	char path_max_tmp[MPD_PATH_MAX];
	Directory *parentDirectory;

	parent = parent_path(path_max_tmp, utf8path);

	if (strlen(parent) == 0)
		parentDirectory = (void *)mp3rootDirectory;
	else
		parentDirectory = addDirectoryPathToDB(parent, shortname);

	if (!parentDirectory)
		return NULL;

	*shortname = utf8path + strlen(parent);
	while (*(*shortname) && *(*shortname) == '/')
		(*shortname)++;

	return (Directory *) parentDirectory;
}

/* return values:
   -1 -> error
    0 -> no error, but nothing updated
    1 -> no error, and stuff updated
 */
static int updatePath(const char *utf8path)
{
	Directory *directory;
	Directory *parentDirectory;
	Song *song;
	const char *shortname;
	char *path = sanitizePathDup(utf8path);
	time_t mtime;
	int ret = 0;
	char path_max_tmp[MPD_PATH_MAX];

	if (NULL == path)
		return -1;

	/* if path is in the DB try to update it, or else delete it */
	if ((directory = getDirectoryDetails(path, &shortname))) {
		parentDirectory = directory->parent;

		/* if this update directory is successfull, we are done */
		if ((ret = updateDirectory(directory)) >= 0) {
			free(path);
			sortDirectory(directory);
			return ret;
		}
		/* we don't want to delete the root directory */
		else if (directory == mp3rootDirectory) {
			free(path);
			return 0;
		}
		/* if updateDirectory fails, means we should delete it */
		else {
			LOG("removing directory: %s\n", path);
			deleteFromList(parentDirectory->subDirectories,
				       shortname);
			ret = 1;
			/* don't return, path maybe a song now */
		}
	} else if ((song = getSongDetails(path, &shortname, &parentDirectory))) {
		if (!parentDirectory->stat
		    && statDirectory(parentDirectory) < 0) {
			free(path);
			return 0;
		}
		/* if this song update is successfull, we are done */
		else if (0 == inodeFoundInParent(parentDirectory->parent,
						 parentDirectory->stat->inode,
						 parentDirectory->stat->device)
			 && song &&
			 isMusic(get_song_url(path_max_tmp, song), &mtime, 0)) {
			free(path);
			if (song->mtime == mtime)
				return 0;
			else if (updateSongInfo(song) == 0)
				return 1;
			else {
				removeSongFromDirectory(parentDirectory,
							shortname);
				return 1;
			}
		}
		/* if updateDirectory fails, means we should delete it */
		else {
			removeSongFromDirectory(parentDirectory, shortname);
			ret = 1;
			/* don't return, path maybe a directory now */
		}
	}

	/* path not found in the db, see if it actually exists on the fs.
	 * Also, if by chance a directory was replaced by a file of the same
	 * name or vice versa, we need to add it to the db
	 */
	if (isDir(path) || isMusic(path, NULL, 0)) {
		parentDirectory = addParentPathToDB(path, &shortname);
		if (!parentDirectory || (!parentDirectory->stat &&
					 statDirectory(parentDirectory) < 0)) {
		} else if (0 == inodeFoundInParent(parentDirectory->parent,
						   parentDirectory->stat->inode,
						   parentDirectory->stat->
						   device)
			   && addToDirectory(parentDirectory, shortname, path)
			   > 0) {
			ret = 1;
		}
	}

	free(path);

	return ret;
}

static const char *opendir_path(char *path_max_tmp, const char *dirname)
{
	if (*dirname != '\0')
		return rmp2amp_r(path_max_tmp,
		                 utf8_to_fs_charset(path_max_tmp, dirname));
	return musicDir;
}

/* return values:
   -1 -> error
    0 -> no error, but nothing updated
    1 -> no error, and stuff updated
 */
static int updateDirectory(Directory * directory)
{
	DIR *dir;
	const char *dirname = getDirectoryPath(directory);
	struct dirent *ent;
	char path_max_tmp[MPD_PATH_MAX];
	int ret = 0;

	if (!directory->stat && statDirectory(directory) < 0)
		return -1;
	else if (inodeFoundInParent(directory->parent,
				    directory->stat->inode,
				    directory->stat->device))
		return -1;

	dir = opendir(opendir_path(path_max_tmp, dirname));
	if (!dir)
		return -1;

	if (removeDeletedFromDirectory(path_max_tmp, directory) > 0)
		ret = 1;

	while ((ent = readdir(dir))) {
		char *utf8;
		if (skip_path(ent->d_name))
			continue;

		utf8 = fs_charset_to_utf8(path_max_tmp, ent->d_name);
		if (!utf8)
			continue;

		if (directory->path)
			utf8 = pfx_dir(path_max_tmp, utf8, strlen(utf8),
			               dirname, strlen(dirname));
		if (updateInDirectory(directory, utf8, path_max_tmp) > 0)
			ret = 1;
	}

	closedir(dir);

	return ret;
}

/* return values:
   -1 -> error
    0 -> no error, but nothing found
    1 -> no error, and stuff found
 */
static int exploreDirectory(Directory * directory)
{
	DIR *dir;
	const char *dirname = getDirectoryPath(directory);
	struct dirent *ent;
	char path_max_tmp[MPD_PATH_MAX];
	int ret = 0;

	DEBUG("explore: attempting to opendir: %s\n", dirname);

	dir = opendir(opendir_path(path_max_tmp, dirname));
	if (!dir)
		return -1;

	DEBUG("explore: %s\n", dirname);

	while ((ent = readdir(dir))) {
		char *utf8;
		if (skip_path(ent->d_name))
			continue;

		utf8 = fs_charset_to_utf8(path_max_tmp, ent->d_name);
		if (!utf8)
			continue;

		DEBUG("explore: found: %s (%s)\n", ent->d_name, utf8);

		if (directory->path)
			utf8 = pfx_dir(path_max_tmp, utf8, strlen(utf8),
			               dirname, strlen(dirname));
		if (addToDirectory(directory, utf8, path_max_tmp) > 0)
			ret = 1;
	}

	closedir(dir);

	return ret;
}

static int statDirectory(Directory * dir)
{
	struct stat st;

	if (myStat(getDirectoryPath(dir), &st) < 0)
		return -1;

	dir->stat = newDirectoryStat(&st);

	return 0;
}

static int inodeFoundInParent(Directory * parent, ino_t inode, dev_t device)
{
	while (parent) {
		if (!parent->stat) {
			if (statDirectory(parent) < 0)
				return -1;
		}
		if (parent->stat->inode == inode &&
		    parent->stat->device == device) {
			DEBUG("recursive directory found\n");
			return 1;
		}
		parent = parent->parent;
	}

	return 0;
}

static int addSubDirectoryToDirectory(Directory * directory,
				      const char *shortname,
				      const char *name, struct stat *st)
{
	Directory *subDirectory;

	if (inodeFoundInParent(directory, st->st_ino, st->st_dev))
		return 0;

	subDirectory = newDirectory(name, directory);
	subDirectory->stat = newDirectoryStat(st);

	if (exploreDirectory(subDirectory) < 1) {
		freeDirectory(subDirectory);
		return 0;
	}

	insertInList(directory->subDirectories, shortname, subDirectory);

	return 1;
}

static int addToDirectory(Directory * directory,
			  const char *shortname, const char *name)
{
	struct stat st;

	if (myStat(name, &st)) {
		DEBUG("failed to stat %s: %s\n", name, strerror(errno));
		return -1;
	}

	if (S_ISREG(st.st_mode) && hasMusicSuffix(name, 0)) {
		Song *song;
		song = addSongToList(directory->songs, shortname, name,
				     SONG_TYPE_FILE, directory);
		if (!song)
			return -1;
		LOG("added %s\n", name);
		return 1;
	} else if (S_ISDIR(st.st_mode)) {
		return addSubDirectoryToDirectory(directory, shortname, name,
						  &st);
	}

	DEBUG("addToDirectory: %s is not a directory or music\n", name);

	return -1;
}

void closeMp3Directory(void)
{
	freeDirectory(mp3rootDirectory);
}

static Directory *findSubDirectory(Directory * directory, const char *name)
{
	void *subDirectory;
	char *duplicated = xstrdup(name);
	char *key;

	key = strtok(duplicated, "/");
	if (!key) {
		free(duplicated);
		return NULL;
	}

	if (findInList(directory->subDirectories, key, &subDirectory)) {
		free(duplicated);
		return (Directory *) subDirectory;
	}

	free(duplicated);
	return NULL;
}

int isRootDirectory(const char *name)
{
	if (name == NULL || name[0] == '\0' || strcmp(name, "/") == 0) {
		return 1;
	}
	return 0;
}

static Directory *getSubDirectory(Directory * directory, const char *name,
				  const char **shortname)
{
	Directory *subDirectory;
	int len;

	if (isRootDirectory(name)) {
		return directory;
	}

	if ((subDirectory = findSubDirectory(directory, name)) == NULL)
		return NULL;

	*shortname = name;

	len = 0;
	while (name[len] != '/' && name[len] != '\0')
		len++;
	while (name[len] == '/')
		len++;

	return getSubDirectory(subDirectory, &(name[len]), shortname);
}

static Directory *getDirectoryDetails(const char *name, const char **shortname)
{
	*shortname = NULL;

	return getSubDirectory(mp3rootDirectory, name, shortname);
}

static Directory *getDirectory(const char *name)
{
	const char *shortname;

	return getSubDirectory(mp3rootDirectory, name, &shortname);
}

static int printDirectoryList(int fd, DirectoryList * directoryList)
{
	ListNode *node = directoryList->firstNode;
	Directory *directory;

	while (node != NULL) {
		directory = (Directory *) node->data;
		fdprintf(fd, "%s%s\n", DIRECTORY_DIR,
			 getDirectoryPath(directory));
		node = node->nextNode;
	}

	return 0;
}

int printDirectoryInfo(int fd, const char *name)
{
	Directory *directory;

	if ((directory = getDirectory(name)) == NULL) {
		commandError(fd, ACK_ERROR_NO_EXIST, "directory not found");
		return -1;
	}

	printDirectoryList(fd, directory->subDirectories);
	printSongInfoFromList(fd, directory->songs);

	return 0;
}

static void writeDirectoryInfo(FILE * fp, Directory * directory)
{
	ListNode *node = (directory->subDirectories)->firstNode;
	Directory *subDirectory;
	int retv;

	if (directory->path) {
		retv = fprintf(fp, "%s%s\n", DIRECTORY_BEGIN,
			  getDirectoryPath(directory));
		if (retv < 0) {
			ERROR("Failed to write data to database file: %s\n",strerror(errno));
			return;
		}
	}

	while (node != NULL) {
		subDirectory = (Directory *) node->data;
		retv = fprintf(fp, "%s%s\n", DIRECTORY_DIR, node->key);
		if (retv < 0) {
			ERROR("Failed to write data to database file: %s\n",strerror(errno));
			return;
		}
		writeDirectoryInfo(fp, subDirectory);
		node = node->nextNode;
	}

	writeSongInfoFromList(fp, directory->songs);

	if (directory->path) {
		retv = fprintf(fp, "%s%s\n", DIRECTORY_END,
			  getDirectoryPath(directory));
		if (retv < 0) {
			ERROR("Failed to write data to database file: %s\n",strerror(errno));
			return;
		}
	}
}

static void readDirectoryInfo(FILE * fp, Directory * directory)
{
	char buffer[MPD_PATH_MAX * 2];
	int bufferSize = MPD_PATH_MAX * 2;
	char *key;
	Directory *subDirectory;
	int strcmpRet;
	char *name;
	ListNode *nextDirNode = directory->subDirectories->firstNode;
	ListNode *nodeTemp;

	while (myFgets(buffer, bufferSize, fp)
	       && 0 != strncmp(DIRECTORY_END, buffer, strlen(DIRECTORY_END))) {
		if (0 == strncmp(DIRECTORY_DIR, buffer, strlen(DIRECTORY_DIR))) {
			key = xstrdup(&(buffer[strlen(DIRECTORY_DIR)]));
			if (!myFgets(buffer, bufferSize, fp))
				FATAL("Error reading db, fgets\n");
			/* for compatibility with db's prior to 0.11 */
			if (0 == strncmp(DIRECTORY_MTIME, buffer,
					 strlen(DIRECTORY_MTIME))) {
				if (!myFgets(buffer, bufferSize, fp))
					FATAL("Error reading db, fgets\n");
			}
			if (strncmp
			    (DIRECTORY_BEGIN, buffer,
			     strlen(DIRECTORY_BEGIN))) {
				FATAL("Error reading db at line: %s\n", buffer);
			}
			name = xstrdup(&(buffer[strlen(DIRECTORY_BEGIN)]));

			while (nextDirNode && (strcmpRet =
					       strcmp(key,
						      nextDirNode->key)) > 0) {
				nodeTemp = nextDirNode->nextNode;
				deleteNodeFromList(directory->subDirectories,
						   nextDirNode);
				nextDirNode = nodeTemp;
			}

			if (NULL == nextDirNode) {
				subDirectory = newDirectory(name, directory);
				insertInList(directory->subDirectories,
					     key, (void *)subDirectory);
			} else if (strcmpRet == 0) {
				subDirectory = (Directory *) nextDirNode->data;
				nextDirNode = nextDirNode->nextNode;
			} else {
				subDirectory = newDirectory(name, directory);
				insertInListBeforeNode(directory->
						       subDirectories,
						       nextDirNode, -1, key,
						       (void *)subDirectory);
			}

			free(name);
			free(key);
			readDirectoryInfo(fp, subDirectory);
		} else if (0 == strncmp(SONG_BEGIN, buffer, strlen(SONG_BEGIN))) {
			readSongInfoIntoList(fp, directory->songs, directory);
		} else {
			FATAL("Unknown line in db: %s\n", buffer);
		}
	}

	while (nextDirNode) {
		nodeTemp = nextDirNode->nextNode;
		deleteNodeFromList(directory->subDirectories, nextDirNode);
		nextDirNode = nodeTemp;
	}
}

static void sortDirectory(Directory * directory)
{
	ListNode *node = directory->subDirectories->firstNode;
	Directory *subDir;

	sortList(directory->subDirectories);
	sortList(directory->songs);

	while (node != NULL) {
		subDir = (Directory *) node->data;
		sortDirectory(subDir);
		node = node->nextNode;
	}
}

int checkDirectoryDB(void)
{
	struct stat st;
	char *dbFile = getDbFile();

	/* Check if the file exists */
	if (access(dbFile, F_OK)) {
		/* If the file doesn't exist, we can't check if we can write
		 * it, so we are going to try to get the directory path, and
		 * see if we can write a file in that */
		char dirPath[MPD_PATH_MAX];
		parent_path(dirPath, dbFile);
		if (*dirPath == '\0')
			strcpy(dirPath, "/");

		/* Check that the parent part of the path is a directory */
		if (stat(dirPath, &st) < 0) {
			ERROR("Couldn't stat parent directory of db file "
			      "\"%s\": %s\n", dbFile, strerror(errno));
			return -1;
		}

		if (!S_ISDIR(st.st_mode)) {
			ERROR("Couldn't create db file \"%s\" because the "
			      "parent path is not a directory\n", dbFile);
			return -1;
		}

		/* Check if we can write to the directory */
		if (access(dirPath, R_OK | W_OK)) {
			ERROR("Can't create db file in \"%s\": %s\n", dirPath,
			      strerror(errno));
			return -1;
		}

		return 0;
	}

	/* Path exists, now check if it's a regular file */
	if (stat(dbFile, &st) < 0) {
		ERROR("Couldn't stat db file \"%s\": %s\n", dbFile,
		      strerror(errno));
		return -1;
	}

	if (!S_ISREG(st.st_mode)) {
		ERROR("db file \"%s\" is not a regular file\n", dbFile);
		return -1;
	}

	/* And check that we can write to it */
	if (access(dbFile, R_OK | W_OK)) {
		ERROR("Can't open db file \"%s\" for reading/writing: %s\n",
		      dbFile, strerror(errno));
		return -1;
	}

	return 0;
}

int writeDirectoryDB(void)
{
	FILE *fp;
	char *dbFile = getDbFile();
	struct stat st;

	DEBUG("removing empty directories from DB\n");
	deleteEmptyDirectoriesInDirectory(mp3rootDirectory);

	DEBUG("sorting DB\n");

	sortDirectory(mp3rootDirectory);

	DEBUG("writing DB\n");

	while (!(fp = fopen(dbFile, "w")) && errno == EINTR);
	if (!fp) {
		ERROR("unable to write to db file \"%s\": %s\n",
		      dbFile, strerror(errno));
		return -1;
	}

	/* block signals when writing the db so we don't get a corrupted db */
	fprintf(fp, "%s\n", DIRECTORY_INFO_BEGIN);
	fprintf(fp, "%s%s\n", DIRECTORY_MPD_VERSION, VERSION);
	fprintf(fp, "%s%s\n", DIRECTORY_FS_CHARSET, getFsCharset());
	fprintf(fp, "%s\n", DIRECTORY_INFO_END);

	writeDirectoryInfo(fp, mp3rootDirectory);

	while (fclose(fp) && errno == EINTR);

	if (stat(dbFile, &st) == 0)
		directory_dbModTime = st.st_mtime;

	return 0;
}

int readDirectoryDB(void)
{
	FILE *fp = NULL;
	char *dbFile = getDbFile();
	struct stat st;

	if (!mp3rootDirectory)
		mp3rootDirectory = newDirectory(NULL, NULL);
	while (!(fp = fopen(dbFile, "r")) && errno == EINTR) ;
	if (fp == NULL) {
		ERROR("unable to open db file \"%s\": %s\n",
		      dbFile, strerror(errno));
		return -1;
	}

	/* get initial info */
	{
		char buffer[100];
		int bufferSize = 100;
		int foundFsCharset = 0;
		int foundVersion = 0;

		if (!myFgets(buffer, bufferSize, fp))
			FATAL("Error reading db, fgets\n");
		if (0 == strcmp(DIRECTORY_INFO_BEGIN, buffer)) {
			while (myFgets(buffer, bufferSize, fp) &&
			       0 != strcmp(DIRECTORY_INFO_END, buffer)) {
				if (0 == strncmp(DIRECTORY_MPD_VERSION, buffer,
						 strlen(DIRECTORY_MPD_VERSION)))
				{
					if (foundVersion)
						FATAL("already found version in db\n");
					foundVersion = 1;
				} else if (0 ==
					   strncmp(DIRECTORY_FS_CHARSET, buffer,
						   strlen
						   (DIRECTORY_FS_CHARSET))) {
					char *fsCharset;
					char *tempCharset;

					if (foundFsCharset)
						FATAL("already found fs charset in db\n");

					foundFsCharset = 1;

					fsCharset = &(buffer[strlen(DIRECTORY_FS_CHARSET)]);
					if ((tempCharset = getConfigParamValue(CONF_FS_CHARSET))
					    && strcmp(fsCharset, tempCharset)) {
						WARNING("Using \"%s\" for the "
							"filesystem charset "
							"instead of \"%s\"\n",
							fsCharset, tempCharset);
						WARNING("maybe you need to "
							"recreate the db?\n");
						setFsCharset(fsCharset);
					}
				} else {
					FATAL("directory: unknown line in db info: %s\n",
					     buffer);
				}
			}
		} else {
			ERROR("db info not found in db file\n");
			ERROR("you should recreate the db using --create-db\n");
			fseek(fp, 0, SEEK_SET);
			return -1;
		}
	}

	DEBUG("reading DB\n");

	readDirectoryInfo(fp, mp3rootDirectory);
	while (fclose(fp) && errno == EINTR) ;

	stats.numberOfSongs = countSongsIn(STDERR_FILENO, NULL);
	stats.dbPlayTime = sumSongTimesIn(STDERR_FILENO, NULL);

	if (stat(dbFile, &st) == 0)
		directory_dbModTime = st.st_mtime;

	return 0;
}

void updateMp3Directory(void)
{
	switch (updateDirectory(mp3rootDirectory)) {
	case 0:
		/* nothing updated */
		return;
	case 1:
		if (writeDirectoryDB() < 0)
			exit(EXIT_FAILURE);
		break;
	default:
		/* something was updated and db should be written */
		FATAL("problems updating music db\n");
	}

	return;
}

static int traverseAllInSubDirectory(int fd, Directory * directory,
				     int (*forEachSong) (int, Song *,
							 void *),
				     int (*forEachDir) (int, Directory *,
							void *), void *data)
{
	ListNode *node = directory->songs->firstNode;
	Song *song;
	Directory *dir;
	int errFlag = 0;

	if (forEachDir) {
		errFlag = forEachDir(fd, directory, data);
		if (errFlag)
			return errFlag;
	}

	if (forEachSong) {
		while (node != NULL && !errFlag) {
			song = (Song *) node->data;
			errFlag = forEachSong(fd, song, data);
			node = node->nextNode;
		}
		if (errFlag)
			return errFlag;
	}

	node = directory->subDirectories->firstNode;

	while (node != NULL && !errFlag) {
		dir = (Directory *) node->data;
		errFlag = traverseAllInSubDirectory(fd, dir, forEachSong,
						    forEachDir, data);
		node = node->nextNode;
	}

	return errFlag;
}

int traverseAllIn(int fd, char *name,
		  int (*forEachSong) (int, Song *, void *),
		  int (*forEachDir) (int, Directory *, void *), void *data)
{
	Directory *directory;

	if ((directory = getDirectory(name)) == NULL) {
		Song *song;
		if ((song = getSongFromDB(name)) && forEachSong) {
			return forEachSong(fd, song, data);
		}
		commandError(fd, ACK_ERROR_NO_EXIST,
			     "directory or file not found");
		return -1;
	}

	return traverseAllInSubDirectory(fd, directory, forEachSong, forEachDir,
					 data);
}

static void freeAllDirectoryStats(Directory * directory)
{
	ListNode *node = directory->subDirectories->firstNode;

	while (node != NULL) {
		freeAllDirectoryStats((Directory *) node->data);
		node = node->nextNode;
	}

	freeDirectoryStatFromDirectory(directory);
}

void initMp3Directory(void)
{
	mp3rootDirectory = newDirectory(NULL, NULL);
	exploreDirectory(mp3rootDirectory);
	freeAllDirectoryStats(mp3rootDirectory);
	stats.numberOfSongs = countSongsIn(STDERR_FILENO, NULL);
	stats.dbPlayTime = sumSongTimesIn(STDERR_FILENO, NULL);
}

static Song *getSongDetails(const char *file, const char **shortnameRet,
			    Directory ** directoryRet)
{
	void *song = NULL;
	Directory *directory;
	char *dir = NULL;
	char *duplicated = xstrdup(file);
	char *shortname = duplicated;
	char *c = strtok(duplicated, "/");

	DEBUG("get song: %s\n", file);

	while (c) {
		shortname = c;
		c = strtok(NULL, "/");
	}

	if (shortname != duplicated) {
		for (c = duplicated; c < shortname - 1; c++) {
			if (*c == '\0')
				*c = '/';
		}
		dir = duplicated;
	}

	if (!(directory = getDirectory(dir))) {
		free(duplicated);
		return NULL;
	}

	if (!findInList(directory->songs, shortname, &song)) {
		free(duplicated);
		return NULL;
	}

	free(duplicated);
	if (shortnameRet)
		*shortnameRet = shortname;
	if (directoryRet)
		*directoryRet = directory;
	return (Song *) song;
}

Song *getSongFromDB(char *file)
{
	return getSongDetails(file, NULL, NULL);
}

time_t getDbModTime(void)
{
	return directory_dbModTime;
}
