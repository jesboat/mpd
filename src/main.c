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

#include "interface.h"
#include "command.h"
#include "playlist.h"
#include "directory.h"
#include "player.h"
#include "listen.h"
#include "conf.h"
#include "path.h"
#include "playerData.h"
#include "stats.h"
#include "sig_handlers.h"
#include "audio.h"
#include "volume.h"
#include "log.h"
#include "permission.h"
#include "replayGain.h"
#include "inputPlugin.h"
#include "inputStream.h"
#include "tag.h"
#include "tagTracker.h"
#include "../config.h"

#include <stdio.h>
#include <sys/select.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <grp.h>
#include <unistd.h>

#define SYSTEM_CONFIG_FILE_LOCATION	"/etc/mpd.conf"
#define USER_CONFIG_FILE_LOCATION	"/.mpdconf"

typedef struct _Options {
        char * portStr;
        char * musicDirArg;
        char * playlistDirArg;
        char * logFile;
        char * errorFile;
        char * usr;
        char * dbFile;
        int daemon;
        int stdOutput;
        int createDB;
	int updateDB;
} Options;

void usage(char * argv[]) {
        ERROR("usage:\n");
        ERROR("   %s [options] <port> <music dir> <playlist dir> <log file> <error file>\n",argv[0]);
        ERROR("   %s [options] <conf file>\n",argv[0]);
        ERROR("   %s [options]   (searches for ~%s then %s)\n",
                        argv[0],USER_CONFIG_FILE_LOCATION,
                        SYSTEM_CONFIG_FILE_LOCATION);
        ERROR("\n");
        ERROR("options:\n");
        ERROR("   --help             this usage statement\n");
        ERROR("   --no-daemon        don't detach from console\n");
        ERROR("   --stdout           print msgs to stdout and stderr\n");
        ERROR("   --create-db        force (re)creation database and exit\n");
        ERROR("   --update-db        create database and exit\n");
        ERROR("   --no-create-db     don't create database\n");
        ERROR("   --verbose          verbose logging\n");
        ERROR("   --version          prints version information\n");
}

void version() {
        LOG("mpd (MPD: Music Player Daemon) %s\n",VERSION);
        LOG("\n");
        LOG("Copyright (C) 2003-2004 Warren Dukes <shank@mercury.chem.pitt.edu>\n");
        LOG("This is free software; see the source for copying conditions.  There is NO\n");
        LOG("warranty; not even MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.\n");
        LOG("\n");
        LOG("Supported formats:\n");

        initInputPlugins();
        printAllInputPluginSuffixes(stdout);
}

void parseOptions(int argc, char ** argv, Options * options) {
        int argcLeft = argc;

        options->usr = NULL;
        options->daemon = 1;
        options->stdOutput = 0;
        options->createDB = 0;
        options->updateDB = 0;
        options->dbFile = NULL;

        if(argc>1) {
                int i = 1;
                while(i<argc) {
                        if(strncmp(argv[i],"--",2)==0) {
                                if(strcmp(argv[i],"--help")==0) {
                                        usage(argv);
                                        exit(EXIT_SUCCESS);
                                }
                                else if(strcmp(argv[i],"--no-daemon")==0) {
                                        options->daemon = 0;
                                        argcLeft--;
                                }
                                else if(strcmp(argv[i],"--stdout")==0) {
                                        options->stdOutput = 1;
                                        argcLeft--;
                                }
                                else if(strcmp(argv[i],"--create-db")==0) {
                                        options->stdOutput = 1;
                                        options->createDB = 1;
                                        argcLeft--;
                                }
                                else if(strcmp(argv[i],"--update-db")==0) {
                                        options->stdOutput = 1;
                                        options->updateDB = 1;
                                        argcLeft--;
                                }
                                else if(strcmp(argv[i],"--no-create-db")==0) {
                                        options->createDB = -1;
                                        argcLeft--;
                                }
                                else if(strcmp(argv[i],"--verbose")==0) {
                                        logLevel = LOG_LEVEL_DEBUG;
                                        argcLeft--;
                                }
                                else if(strcmp(argv[i],"--version")==0) {
                                        version();
                                        exit(EXIT_SUCCESS);
                                }
                                else {
                                        myfprintf(stderr,"unknown command line option: %s\n",argv[i]);
                                        exit(EXIT_FAILURE);
                                }
                        }
                        else break;
                        i++;
                }
        }

        if(argcLeft==6) {
                options->portStr = argv[argc-5];
                options->musicDirArg = argv[argc-4];
                options->playlistDirArg = argv[argc-3];
                options->logFile = argv[argc-2];
                options->errorFile = argv[argc-1];
                return;
        }
        else if(argcLeft<=2) {
                int conf = 0;
                if(argcLeft==2) {
			readConf(argv[argc-1]);
			conf = 1;
		}
                else if(argcLeft==1) {
                        FILE * fp;
                        char * homedir = getenv("HOME");
                        char userfile[MAXPATHLEN+1] = "";
                        if(homedir && (strlen(homedir)+
                                                strlen(USER_CONFIG_FILE_LOCATION)) <
                                        MAXPATHLEN) {
                                strcpy(userfile,homedir);
                                strcat(userfile,USER_CONFIG_FILE_LOCATION);
                        }
                        if(strlen(userfile) && (fp=fopen(userfile,"r"))) {
                                fclose(fp);
                                readConf(userfile);
				conf = 1;
                        }
                        else if((fp=fopen(SYSTEM_CONFIG_FILE_LOCATION,"r"))) {
                                fclose(fp);
                                readConf(SYSTEM_CONFIG_FILE_LOCATION);
				conf = 1;
                        }
                }
                if(conf) {
                        options->portStr = forceAndGetConfigParamValue(
					CONF_PORT);
                        options->musicDirArg = 
				parseConfigFilePath(CONF_MUSIC_DIR, 1);
                        options->playlistDirArg = 
				parseConfigFilePath(CONF_PLAYLIST_DIR, 1);
                        options->logFile = parseConfigFilePath(CONF_LOG_FILE,1);
                        options->errorFile = 
				parseConfigFilePath(CONF_ERROR_FILE, 1);
                        options->usr = getConfigParamValue(CONF_USER);
                        options->dbFile = parseConfigFilePath(CONF_DB_FILE, 0);
                        return;
                }
        }

        usage(argv);
        exit(EXIT_FAILURE);
}

void closeAllFDs() {
        int i;
	int fds = getdtablesize();

        for(i = 3; i < fds; i++) close(i);
}

void establishListen(Options * options) {
        int port;

        if((port = atoi(options->portStr))<0) {
                ERROR("problem with port number\n");
                exit(EXIT_FAILURE);
        }

        if(options->createDB <= 0 && !options->updateDB) establish(port);
}

void changeToUser(Options * options) {
        if (options->usr && strlen(options->usr)) {
                /* get uid */
                struct passwd * userpwd;
                if ((userpwd = getpwnam(options->usr)) == NULL) {
                        ERROR("no such user: %s\n", options->usr);
                        exit(EXIT_FAILURE);
                }

                if(setgid(userpwd->pw_gid) == -1) {
                        ERROR("cannot setgid of user %s: %s\n", options->usr,
                                        strerror(errno));
                        exit(EXIT_FAILURE);
                }

#ifdef _BSD_SOURCE
                /* init suplementary groups 
                 * (must be done before we change our uid)
                 */
                if (initgroups(options->usr, userpwd->pw_gid) == -1) {
                        WARNING("cannot init suplementary groups "
                                        "of user %s: %s\n", options->usr, 
                                        strerror(errno));
                }
#endif

                /* set uid */
                if (setuid(userpwd->pw_uid) == -1) {
                        ERROR("cannot change to uid of user "
                                        "%s: %s\n", options->usr, 
                                        strerror(errno));
                        exit(EXIT_FAILURE);
                }

		if(userpwd->pw_dir) {
			setenv("HOME", userpwd->pw_dir, 1);
		}
        }
}

void openLogFiles(Options * options, FILE ** out, FILE ** err) {
        mode_t prev;

        if(options->stdOutput) {
                flushWarningLog();
                return;
        }

        /* be sure to create log files w/ rw permissions*/
        prev = umask(0066);

        if(NULL==(*out=fopen(options->logFile,"a"))) {
                ERROR("problem opening file \"%s\" for writing\n",
                                options->logFile);
                exit(EXIT_FAILURE);
        }

        if(NULL==(*err=fopen(options->errorFile,"a"))) {
                ERROR("problem opening file \"%s\" for writing\n",
                                options->errorFile);
                exit(EXIT_FAILURE);
        }

        umask(prev);
}

void openDB(Options * options, char * argv0) {
        if(!options->dbFile) directory_db = strdup(rpp2app(".mpddb"));
        else directory_db = strdup(options->dbFile);

        if(options->createDB>0 || readDirectoryDB()<0) {
                if(options->createDB<0) {
                        ERROR("can't open db file and using \"--no-create-db\""
                                        " command line option\n");
			ERROR("try running \"%s --create-db\"\n",
					argv0);
                        exit(EXIT_FAILURE);
                }
                flushWarningLog();
                initMp3Directory();
                if(writeDirectoryDB()<0) {
                        ERROR("problem opening db for reading or writing\n");
                        exit(EXIT_FAILURE);
                }
		if(options->createDB) exit(EXIT_SUCCESS);
        }
	if(options->updateDB) {
                flushWarningLog();
		updateMp3Directory();
		exit(EXIT_SUCCESS);
	}
}

void daemonize(Options * options) {
        if(options->daemon) {
                int pid;

                fflush(NULL);
                pid = fork();
                if(pid>0) _exit(EXIT_SUCCESS);
                else if(pid<0) {
                        ERROR("problems fork'ing for daemon!\n");
                        exit(EXIT_FAILURE);
                }

                if(chdir("/")<0) {
                        ERROR("problems changing to root directory\n");
                        exit(EXIT_FAILURE);
                }

                if(setsid()<0) {
                        ERROR("problems setsid'ing\n");
                        exit(EXIT_FAILURE);
                }

                fflush(NULL);
                pid = fork();
                if(pid>0) _exit(EXIT_SUCCESS);
                else if(pid<0) {
                        ERROR("problems fork'ing for daemon!\n");
                        exit(EXIT_FAILURE);
                }
        }
}

void setupLogOutput(Options * options, FILE * out, FILE * err) {
        if(!options->stdOutput) {
                fflush(NULL);

                if(dup2(fileno(out),STDOUT_FILENO)<0) {
                        myfprintf(err,"problems dup2 stdout : %s\n",
                                        strerror(errno));
                        exit(EXIT_FAILURE);
                }

                if(dup2(fileno(err),STDERR_FILENO)<0) {
                        myfprintf(err,"problems dup2 stderr : %s\n",
                                        strerror(errno));
                        exit(EXIT_FAILURE);
                }

                myfprintfStdLogMode(out, err, options->logFile,
                                options->errorFile);
                flushWarningLog();
        }

        /* lets redirect stdin to dev null as a work around for libao bug */
        {
                int fd = open("/dev/null",O_RDONLY);
                if(fd<0) {
                        ERROR("not able to open /dev/null to redirect stdin: "
                                        "%s\n",strerror(errno));
                        exit(EXIT_FAILURE);
                }
                if(dup2(fd,STDIN_FILENO)<0) {
                        ERROR("problems dup2's stdin for redirection: "
                                        "%s\n",strerror(errno));
                        exit(EXIT_FAILURE);
                }
        }
}

int main(int argc, char * argv[]) {
        FILE * out;
        FILE * err;
        Options options;

        closeAllFDs();

        initConf();

        parseOptions(argc, argv, &options);

        initStats();
	initTagConfig();
        initLog();

        establishListen(&options);

        changeToUser(&options);

        openLogFiles(&options, &out, &err);

	initPaths(options.playlistDirArg,options.musicDirArg);
	initPermissions();
        initReplayGainState();

        initPlaylist();
        initInputPlugins();

        openDB(&options, argv[0]);

        initCommands();
        initPlayerData();
        initAudioConfig();
        initAudioDriver();
        initVolume();
        initInterfaces();
	initInputStream(); 

	printMemorySavedByTagTracker();
	
        daemonize(&options);

        setupLogOutput(&options, out, err);

        openVolumeDevice();
        initSigHandlers();
        readPlaylistState();

        while(COMMAND_RETURN_KILL!=doIOForInterfaces()) {
                syncPlayerAndPlaylist();
                closeOldInterfaces();
		if(COMMAND_RETURN_KILL==handlePendingSignals()) break;
		readDirectoryDBIfUpdateIsFinished();
        }

        savePlaylistState();
        playerKill();

        freeAllInterfaces();
	closeAllListenSockets();
        closeMp3Directory();
        finishPlaylist();
        freePlayerData();
        finishAudioDriver();
        finishAudioConfig();
        finishVolume();
	finishPaths();
	finishPermissions();
        finishCommands();
        finishInputPlugins();

        return EXIT_SUCCESS;
}
