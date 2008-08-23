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

#include "playlist.h"
#include "player_error.h"
#include "command.h"
#include "ls.h"
#include "tag.h"
#include "conf.h"
#include "directory.h"
#include "log.h"
#include "path.h"
#include "utils.h"
#include "state_file.h"
#include "storedPlaylist.h"
#include "ack.h"
#include "myfprintf.h"
#include "os_compat.h"
#include "main_notify.h"

enum _playlist_state {
	PLAYLIST_STATE_STOP = 0,
	PLAYLIST_STATE_PLAY = 1
};
static enum _playlist_state playlist_state;

struct _playlist {
	Song **songs;
	/* holds version a song was modified on */
	mpd_uint32 *songMod;
	int *order;
	int *positionToId;
	int *idToPosition;
	int length;
	int current;
	int queued; /* to be decoded */
	int repeat;
	int random;
	mpd_uint32 version;
};

#define PLAYLIST_PREV_UNLESS_ELAPSED    10

#define PLAYLIST_STATE_FILE_STATE		"state: "
#define PLAYLIST_STATE_FILE_RANDOM		"random: "
#define PLAYLIST_STATE_FILE_REPEAT		"repeat: "
#define PLAYLIST_STATE_FILE_CURRENT		"current: "
#define PLAYLIST_STATE_FILE_TIME		"time: "
#define PLAYLIST_STATE_FILE_CROSSFADE		"crossfade: "
#define PLAYLIST_STATE_FILE_PLAYLIST_BEGIN	"playlist_begin"
#define PLAYLIST_STATE_FILE_PLAYLIST_END	"playlist_end"

#define PLAYLIST_STATE_FILE_STATE_PLAY		"play"
#define PLAYLIST_STATE_FILE_STATE_PAUSE		"pause"
#define PLAYLIST_STATE_FILE_STATE_STOP		"stop"

#define PLAYLIST_BUFFER_SIZE	2*MPD_PATH_MAX

#define PLAYLIST_HASH_MULT	4

#define DEFAULT_PLAYLIST_MAX_LENGTH		(1024*16)
#define DEFAULT_PLAYLIST_SAVE_ABSOLUTE_PATHS	0

static struct _playlist playlist;
int playlist_max_length = DEFAULT_PLAYLIST_MAX_LENGTH;
static int playlist_stopOnError;
static int playlist_errorCount;

/*
 * queue_lock is to prevent ourselves from modifying playlist.queued
 * while the decoder is decoding the song.  The main_thread in mpd is
 * the only modifier of playlist.queued.  However, we may modify
 * playlist.queued "in-place" without locking if it points to the same
 * song (during move or shuffle).
 */
static pthread_mutex_t queue_lock = PTHREAD_MUTEX_INITIALIZER;

int playlist_saveAbsolutePaths = DEFAULT_PLAYLIST_SAVE_ABSOLUTE_PATHS;

static void swapOrder(int a, int b);
static int play_order_num(int fd, int order_num, float seek_time);
static void randomizeOrder(int start, int end);

static void incrPlaylistVersion(void)
{
	static unsigned long max = ((mpd_uint32) 1 << 31) - 1;
	playlist.version++;
	if (playlist.version >= max) {
		int i;

		for (i = 0; i < playlist.length; i++) {
			playlist.songMod[i] = 0;
		}

		playlist.version = 1;
	}
}

void playlistVersionChange(void)
{
	int i;

	for (i = 0; i < playlist.length; i++) {
		playlist.songMod[i] = playlist.version;
	}

	incrPlaylistVersion();
}

static void incrPlaylistCurrent(void)
{
	if (playlist.current < 0)
		return;

	if (playlist.current >= playlist.length - 1) {
		if (playlist.repeat)
			playlist.current = 0;
		else
			playlist.current = -1;
	} else
		playlist.current++;
}

void initPlaylist(void)
{
	char *test;
	int i;
	ConfigParam *param;

	playlist.length = 0;
	playlist.repeat = 0;
	playlist.version = 1;
	playlist.random = 0;
	playlist.queued = -1;
	playlist.current = -1;

	param = getConfigParam(CONF_MAX_PLAYLIST_LENGTH);

	if (param) {
		playlist_max_length = strtol(param->value, &test, 10);
		if (*test != '\0') {
			FATAL("max playlist length \"%s\" is not an integer, "
			      "line %i\n", param->value, param->line);
		}
	}

	playlist_saveAbsolutePaths = getBoolConfigParam(
	                                         CONF_SAVE_ABSOLUTE_PATHS, 1);
	if (playlist_saveAbsolutePaths == CONF_BOOL_UNSET)
		playlist_saveAbsolutePaths =
		                         DEFAULT_PLAYLIST_SAVE_ABSOLUTE_PATHS;

	playlist.songs = xmalloc(sizeof(Song *) * playlist_max_length);
	playlist.songMod = xmalloc(sizeof(mpd_uint32) * playlist_max_length);
	playlist.order = xmalloc(sizeof(int) * playlist_max_length);
	playlist.idToPosition = xmalloc(sizeof(int) * playlist_max_length *
				       PLAYLIST_HASH_MULT);
	playlist.positionToId = xmalloc(sizeof(int) * playlist_max_length);

	memset(playlist.songs, 0, sizeof(char *) * playlist_max_length);

	srandom(time(NULL));

	for (i = 0; i < playlist_max_length * PLAYLIST_HASH_MULT; i++) {
		playlist.idToPosition[i] = -1;
	}
}

static int getNextId(void)
{
	static int cur = -1;

	do {
		cur++;
		if (cur >= playlist_max_length * PLAYLIST_HASH_MULT) {
			cur = 0;
		}
	} while (playlist.idToPosition[cur] != -1);

	return cur;
}

void finishPlaylist(void)
{
	int i;
	for (i = 0; i < playlist.length; i++) {
		if (playlist.songs[i]->type == SONG_TYPE_URL) {
			freeJustSong(playlist.songs[i]);
		}
	}

	playlist.length = 0;

	free(playlist.songs);
	playlist.songs = NULL;
	free(playlist.songMod);
	playlist.songMod = NULL;
	free(playlist.order);
	playlist.order = NULL;
	free(playlist.idToPosition);
	playlist.idToPosition = NULL;
	free(playlist.positionToId);
	playlist.positionToId = NULL;
}

int clearPlaylist(int fd)
{
	int i;

	if (stopPlaylist(fd) < 0)
		return -1;

	for (i = 0; i < playlist.length; i++) {
		if (playlist.songs[i]->type == SONG_TYPE_URL) {
			freeJustSong(playlist.songs[i]);
		}
		playlist.idToPosition[playlist.positionToId[i]] = -1;
		playlist.songs[i] = NULL;
	}
	playlist.length = 0;
	playlist.current = -1;

	incrPlaylistVersion();

	return 0;
}

int clearStoredPlaylist(int fd, char *utf8file)
{
	return removeAllFromStoredPlaylistByPath(fd, utf8file);
}

int showPlaylist(int fd)
{
	int i;
	char path_max_tmp[MPD_PATH_MAX];

	for (i = 0; i < playlist.length; i++) {
		fdprintf(fd, "%i:%s\n", i,
		         get_song_url(path_max_tmp, playlist.songs[i]));
	}

	return 0;
}

void savePlaylistState(FILE *fp)
{
	fprintf(fp, "%s", PLAYLIST_STATE_FILE_STATE);
	switch (playlist_state) {
	case PLAYLIST_STATE_PLAY:
		switch (ob_get_state()) {
		case OB_STATE_PAUSE:
			fprintf(fp, "%s\n", PLAYLIST_STATE_FILE_STATE_PAUSE);
			break;
		default:
			fprintf(fp, "%s\n", PLAYLIST_STATE_FILE_STATE_PLAY);
		}
		fprintf(fp, "%s%i\n", PLAYLIST_STATE_FILE_CURRENT,
		        playlist.order[playlist.current]);
		fprintf(fp, "%s%lu\n", PLAYLIST_STATE_FILE_TIME,
		        ob_get_elapsed_time());
		break;
	default:
		fprintf(fp, "%s\n", PLAYLIST_STATE_FILE_STATE_STOP);
		break;
	}
	fprintf(fp, "%s%i\n", PLAYLIST_STATE_FILE_RANDOM, playlist.random);
	fprintf(fp, "%s%i\n", PLAYLIST_STATE_FILE_REPEAT, playlist.repeat);
	fprintf(fp, "%s%i\n", PLAYLIST_STATE_FILE_CROSSFADE,
	        (int)(ob_get_xfade()));
	fprintf(fp, "%s\n", PLAYLIST_STATE_FILE_PLAYLIST_BEGIN);
	fflush(fp);
	showPlaylist(fileno(fp));
	fprintf(fp, "%s\n", PLAYLIST_STATE_FILE_PLAYLIST_END);
}

static void loadPlaylistFromStateFile(FILE *fp, char *buffer,
				      enum ob_state state,
				      int current, int seek_time)
{
	char *temp;
	int song;

	if (!myFgets(buffer, PLAYLIST_BUFFER_SIZE, fp))
		state_file_fatal();
	while (strcmp(buffer, PLAYLIST_STATE_FILE_PLAYLIST_END)) {
		temp = strtok(buffer, ":");
		if (temp == NULL)
			state_file_fatal();
		song = atoi(temp);
		if (!(temp = strtok(NULL, "")))
			state_file_fatal();
		if (!addToPlaylist(STDERR_FILENO, temp, NULL)
		    && current == song) {
			if (state == OB_STATE_PAUSE)
				ob_trigger_action(OB_ACTION_PAUSE_SET);
			if (state != OB_STATE_STOP) {
				seekSongInPlaylist(STDERR_FILENO,
						   playlist.length - 1,
						   seek_time);
			}
		}
		if (!myFgets(buffer, PLAYLIST_BUFFER_SIZE, fp))
			state_file_fatal();
	}
}

void readPlaylistState(FILE *fp)
{
	int current = -1;
	int seek_time = 0;
	enum ob_state state = OB_STATE_STOP;
	char buffer[PLAYLIST_BUFFER_SIZE];

	while (myFgets(buffer, PLAYLIST_BUFFER_SIZE, fp)) {
		if (strncmp(buffer, PLAYLIST_STATE_FILE_STATE,
			    strlen(PLAYLIST_STATE_FILE_STATE)) == 0) {
			if (strcmp(&(buffer[strlen(PLAYLIST_STATE_FILE_STATE)]),
				   PLAYLIST_STATE_FILE_STATE_PLAY) == 0) {
				state = OB_STATE_PLAY;
			} else
			    if (strcmp
				(&(buffer[strlen(PLAYLIST_STATE_FILE_STATE)]),
				 PLAYLIST_STATE_FILE_STATE_PAUSE)
				== 0) {
				state = OB_STATE_PAUSE;
			}
		} else if (strncmp(buffer, PLAYLIST_STATE_FILE_TIME,
				   strlen(PLAYLIST_STATE_FILE_TIME)) == 0) {
			seek_time =
			    atoi(&(buffer[strlen(PLAYLIST_STATE_FILE_TIME)]));
		} else
		    if (strncmp
			(buffer, PLAYLIST_STATE_FILE_REPEAT,
			 strlen(PLAYLIST_STATE_FILE_REPEAT)) == 0) {
			if (strcmp
			    (&(buffer[strlen(PLAYLIST_STATE_FILE_REPEAT)]),
			     "1") == 0) {
				setPlaylistRepeatStatus(STDERR_FILENO, 1);
			} else
				setPlaylistRepeatStatus(STDERR_FILENO, 0);
		} else
		    if (strncmp
			(buffer, PLAYLIST_STATE_FILE_CROSSFADE,
			 strlen(PLAYLIST_STATE_FILE_CROSSFADE)) == 0) {
			ob_set_xfade(atoi(&(buffer
					     [strlen
					      (PLAYLIST_STATE_FILE_CROSSFADE)])));
		} else
		    if (strncmp
			(buffer, PLAYLIST_STATE_FILE_RANDOM,
			 strlen(PLAYLIST_STATE_FILE_RANDOM)) == 0) {
			if (strcmp
			    (&
			     (buffer
			      [strlen(PLAYLIST_STATE_FILE_RANDOM)]),
			     "1") == 0) {
				setPlaylistRandomStatus(STDERR_FILENO, 1);
			} else
				setPlaylistRandomStatus(STDERR_FILENO, 0);
		} else if (strncmp(buffer, PLAYLIST_STATE_FILE_CURRENT,
				   strlen(PLAYLIST_STATE_FILE_CURRENT))
			   == 0) {
			if (strlen(buffer) ==
			    strlen(PLAYLIST_STATE_FILE_CURRENT))
				state_file_fatal();
			current = atoi(&(buffer
					 [strlen
					  (PLAYLIST_STATE_FILE_CURRENT)]));
		} else
		    if (strncmp
			(buffer, PLAYLIST_STATE_FILE_PLAYLIST_BEGIN,
			 strlen(PLAYLIST_STATE_FILE_PLAYLIST_BEGIN)
			) == 0) {
			if (state == OB_STATE_STOP)
				current = -1;
			loadPlaylistFromStateFile(fp, buffer, state,
						  current, seek_time);
		}
	}
}

static void printPlaylistSongInfo(int fd, int song)
{
	printSongInfo(fd, playlist.songs[song]);
	fdprintf(fd, "Pos: %i\nId: %i\n", song, playlist.positionToId[song]);
}

int playlistChanges(int fd, mpd_uint32 version)
{
	int i;

	for (i = 0; i < playlist.length; i++) {
		if (version > playlist.version ||
		    playlist.songMod[i] >= version ||
		    playlist.songMod[i] == 0) {
			printPlaylistSongInfo(fd, i);
		}
	}

	return 0;
}

int playlistChangesPosId(int fd, mpd_uint32 version)
{
	int i;

	for (i = 0; i < playlist.length; i++) {
		if (version > playlist.version ||
		    playlist.songMod[i] >= version ||
		    playlist.songMod[i] == 0) {
			fdprintf(fd, "cpos: %i\nId: %i\n",
			         i, playlist.positionToId[i]);
		}
	}

	return 0;
}

int playlistInfo(int fd, int song)
{
	int i;
	int begin = 0;
	int end = playlist.length;

	if (song >= 0) {
		begin = song;
		end = song + 1;
	}
	if (song >= playlist.length) {
		commandError(fd, ACK_ERROR_NO_EXIST,
			     "song doesn't exist: \"%i\"", song);
		return -1;
	}

	for (i = begin; i < end; i++)
		printPlaylistSongInfo(fd, i);

	return 0;
}

# define checkSongId(id) { \
	if(id < 0 || id >= PLAYLIST_HASH_MULT*playlist_max_length || \
			playlist.idToPosition[id] == -1 ) \
	{ \
		commandError(fd, ACK_ERROR_NO_EXIST, \
			"song id doesn't exist: \"%i\"", id); \
		return -1; \
	} \
}

int playlistId(int fd, int id)
{
	int i;
	int begin = 0;
	int end = playlist.length;

	if (id >= 0) {
		checkSongId(id);
		begin = playlist.idToPosition[id];
		end = begin + 1;
	}

	for (i = begin; i < end; i++)
		printPlaylistSongInfo(fd, i);

	return 0;
}

static void swapSongs(int song1, int song2)
{
	Song *sTemp;
	int iTemp;

	sTemp = playlist.songs[song1];
	playlist.songs[song1] = playlist.songs[song2];
	playlist.songs[song2] = sTemp;

	playlist.songMod[song1] = playlist.version;
	playlist.songMod[song2] = playlist.version;

	playlist.idToPosition[playlist.positionToId[song1]] = song2;
	playlist.idToPosition[playlist.positionToId[song2]] = song1;

	iTemp = playlist.positionToId[song1];
	playlist.positionToId[song1] = playlist.positionToId[song2];
	playlist.positionToId[song2] = iTemp;
}

static Song *song_at(int order_num)
{
	if (order_num >= 0 && order_num < playlist.length) {
		assert(playlist.songs[playlist.order[order_num]]);
		return playlist.songs[playlist.order[order_num]];
	}
	return NULL;
}

static int next_order_num(void)
{
	if (playlist.current < playlist.length - 1) {
		return playlist.current + 1;
	} else if (playlist.length && playlist.repeat) {
		if (playlist.length > 1 && playlist.random)
			randomizeOrder(0, playlist.length - 1);
		return 0;
	}
	return -1;
}

static void queueNextSongInPlaylist(void)
{
	if (playlist_state != PLAYLIST_STATE_PLAY)
		return;
	/* DEBUG("%s:%d\n", __func__, __LINE__); */
	if (pthread_mutex_trylock(&queue_lock) == EBUSY)
		return; /* still decoding */
	DEBUG("%s:%d\n", __func__, __LINE__);
	playlist.queued = next_order_num();
	pthread_mutex_unlock(&queue_lock);
	if (playlist.queued < 0) {
		playlist_state = PLAYLIST_STATE_STOP;
	} else if (dc.state == DC_STATE_STOP) {
		/* DEBUG("%s:%d (%d)\n", __func__, __LINE__, playlist.queued);*/
		dc_trigger_action(DC_ACTION_START, 0);
	}
}

static void syncPlaylistWithQueue(void)
{
	assert(playlist_state == PLAYLIST_STATE_PLAY);

	if (!ob_synced())
		return;

	if (playlist.queued >= 0 &&
	    playlist.current != playlist.queued) {
		DEBUG("playlist: now playing queued song\n");
		DEBUG("%s:%d queued: %d\n",__func__,__LINE__,playlist.queued);
		playlist.current = playlist.queued;
	}
	queueNextSongInPlaylist();
}

void playlist_queue_next(void)
{
	assert(pthread_equal(pthread_self(), dc.thread));
	pthread_mutex_unlock(&queue_lock);
	wakeup_main_task();
}

Song *playlist_queued_song(void)
{
	assert(pthread_equal(pthread_self(), dc.thread));
	pthread_mutex_lock(&queue_lock);
	return song_at(playlist.queued);
}

static void queue_song_locked(int order_num)
{
	pthread_mutex_lock(&queue_lock);
	playlist.queued = order_num;
	pthread_mutex_unlock(&queue_lock);
}

/*
 * stops decoder iff we're decoding a song we haven't played yet
 * Returns the currently queued song, -1 if we cleared the queue
 * This will not affect the currently playing song
 */
static int clear_queue(void)
{
	if (playlist.queued >= 0 && playlist.current != playlist.queued) {
		dc_trigger_action(DC_ACTION_STOP, 0);
		assert(dc.state == DC_STATE_STOP);
		ob_drop_audio(OB_DROP_DECODED);
		queue_song_locked(-1);
	}
	return playlist.queued;
}

int addToPlaylist(int fd, char *url, int *added_id)
{
	Song *song;

	DEBUG("add to playlist: %s\n", url);

	if ((song = getSongFromDB(url))) {
	} else if (!(isValidRemoteUtf8Url(url) &&
		     (song = newSong(url, SONG_TYPE_URL, NULL)))) {
		commandError(fd, ACK_ERROR_NO_EXIST,
			     "\"%s\" is not in the music db or is "
			     "not a valid url", url);
		return -1;
	}

	return addSongToPlaylist(fd, song, added_id);
}

int addToStoredPlaylist(int fd, char *url, char *utf8file)
{
	Song *song;

	DEBUG("add to stored playlist: %s\n", url);

	song = getSongFromDB(url);
	if (song) {
		appendSongToStoredPlaylistByPath(fd, utf8file, song);
		return 0;
	}

	if (!isValidRemoteUtf8Url(url))
		goto fail;

	song = newSong(url, SONG_TYPE_URL, NULL);
	if (song) {
		appendSongToStoredPlaylistByPath(fd, utf8file, song);
		freeJustSong(song);
		return 0;
	}

fail:
	commandError(fd, ACK_ERROR_NO_EXIST, "\"%s\" is not in the music db"
	             "or is not a valid url", url);
	return -1;
}

int addSongToPlaylist(int fd, Song * song, int *added_id)
{
	int id;

	if (playlist.length == playlist_max_length) {
		commandError(fd, ACK_ERROR_PLAYLIST_MAX,
			     "playlist is at the max size");
		return -1;
	}

	if (playlist_state == PLAYLIST_STATE_PLAY) {
		if (playlist.queued >= 0
		    && playlist.current == playlist.length - 1)
			clear_queue();
	}

	id = getNextId();

	playlist.songs[playlist.length] = song;
	playlist.songMod[playlist.length] = playlist.version;
	playlist.order[playlist.length] = playlist.length;
	playlist.positionToId[playlist.length] = id;
	playlist.idToPosition[playlist.positionToId[playlist.length]] =
	    playlist.length;
	playlist.length++;

	if (playlist.random) {
		int swap;
		int start;
		if (playlist.queued >= 0)
			start = playlist.queued + 1;
		else
			start = playlist.current + 1;
		if (start < playlist.length) {
			swap = random() % (playlist.length - start);
			swap += start;
			swapOrder(playlist.length - 1, swap);
		}
	}

	incrPlaylistVersion();

	if (added_id)
		*added_id = id;

	return 0;
}

int swapSongsInPlaylist(int fd, int song1, int song2)
{
	int queuedSong = -1;
	int currentSong;

	if (song1 < 0 || song1 >= playlist.length) {
		commandError(fd, ACK_ERROR_NO_EXIST,
			     "song doesn't exist: \"%i\"", song1);
		return -1;
	}
	if (song2 < 0 || song2 >= playlist.length) {
		commandError(fd, ACK_ERROR_NO_EXIST,
			     "song doesn't exist: \"%i\"", song2);
		return -1;
	}

	if (playlist_state == PLAYLIST_STATE_PLAY) {
		if (playlist.queued >= 0) {
			queuedSong = playlist.order[playlist.queued];
		}
		assert(playlist.current >= 0 &&
		       playlist.current < playlist.length);
		currentSong = playlist.order[playlist.current];

		if (queuedSong == song1 || queuedSong == song2
		    || currentSong == song1 || currentSong == song2)
			clear_queue();
	}

	swapSongs(song1, song2);
	if (playlist.random) {
		int i;
		int k;
		int j = -1;
		for (i = 0; playlist.order[i] != song1; i++) {
			if (playlist.order[i] == song2)
				j = i;
		}
		k = i;
		for (; j == -1; i++)
			if (playlist.order[i] == song2)
				j = i;
		swapOrder(k, j);
	} else {
		if (playlist.current == song1)
			playlist.current = song2;
		else if (playlist.current == song2)
			playlist.current = song1;
	}

	incrPlaylistVersion();

	return 0;
}

int swapSongsInPlaylistById(int fd, int id1, int id2)
{
	checkSongId(id1);
	checkSongId(id2);

	return swapSongsInPlaylist(fd, playlist.idToPosition[id1],
				   playlist.idToPosition[id2]);
}

#define moveSongFromTo(from, to) { \
	playlist.idToPosition[playlist.positionToId[from]] = to; \
	playlist.positionToId[to] = playlist.positionToId[from]; \
	playlist.songs[to] = playlist.songs[from]; \
	playlist.songMod[to] = playlist.version; \
}

int deleteFromPlaylist(int fd, int song)
{
	int i;
	int songOrder;
	int stop_current = 0;
	int prev_queued = playlist.queued;

	if (song < 0 || song >= playlist.length) {
		commandError(fd, ACK_ERROR_NO_EXIST,
			     "song doesn't exist: \"%i\"", song);
		return -1;
	}

	if (playlist_state == PLAYLIST_STATE_PLAY) {
		if (prev_queued >= 0
		    && (playlist.order[prev_queued] == song
			|| playlist.order[playlist.current] == song)) {
			/* DEBUG(__FILE__": %d (clearing)\n", __LINE__); */
			clear_queue();
		}
	}

	if (playlist.songs[song]->type == SONG_TYPE_URL) {
		freeJustSong(playlist.songs[song]);
	}

	playlist.idToPosition[playlist.positionToId[song]] = -1;

	/* delete song from songs array */
	for (i = song; i < playlist.length - 1; i++) {
		moveSongFromTo(i + 1, i);
	}
	/* now find it in the order array */
	for (i = 0; i < playlist.length - 1; i++) {
		if (playlist.order[i] == song)
			break;
	}
	songOrder = i;
	/* delete the entry from the order array */
	for (; i < playlist.length - 1; i++)
		playlist.order[i] = playlist.order[i + 1];
	/* readjust values in the order array */
	for (i = 0; i < playlist.length - 1; i++) {
		if (playlist.order[i] > song)
			playlist.order[i]--;
	}
	/* now take care of other misc stuff */
	playlist.songs[playlist.length - 1] = NULL;
	playlist.length--;

	incrPlaylistVersion();

	/* DEBUG("current: %d, songOrder: %d\n", playlist.current, songOrder); */
	/* DEBUG("playlist_state: %d\n", playlist_state); */
	if (playlist_state != PLAYLIST_STATE_STOP
	    && playlist.current == songOrder)
		stop_current = 1;

	if (playlist.current > songOrder) {
		playlist.current--;
	} else if (playlist.current >= playlist.length) {
		incrPlaylistCurrent();
	}
	if (stop_current) {
		/* DEBUG(__FILE__": %d\n", __LINE__); */
		if (playlist.current >= 0)
			play_order_num(fd, playlist.current, 0);
		else
			stopPlaylist(fd);
	} else {
		/* DEBUG(__FILE__": %d\n", __LINE__); */
		queueNextSongInPlaylist();
	}

	return 0;
}

int deleteFromPlaylistById(int fd, int id)
{
	checkSongId(id);

	return deleteFromPlaylist(fd, playlist.idToPosition[id]);
}

void deleteASongFromPlaylist(Song * song)
{
	int i;

	if (NULL == playlist.songs)
		return;

	for (i = 0; i < playlist.length; i++) {
		if (song == playlist.songs[i]) {
			deleteFromPlaylist(STDERR_FILENO, i);
		}
	}
}

int stopPlaylist(int fd)
{
	DEBUG("playlist: stop\n");

	DEBUG("%s:%d\n", __func__, __LINE__);
	dc_trigger_action(DC_ACTION_STOP, 0);
	DEBUG("%s:%d\n", __func__, __LINE__);
	assert(dc.state == DC_STATE_STOP);
	DEBUG("%s:%d\n", __func__, __LINE__);
	ob_trigger_action(OB_ACTION_STOP);
	assert(ob_get_state() == OB_STATE_STOP);

	DEBUG("%s:%d\n", __func__, __LINE__);
	queue_song_locked(-1);
	playlist_state = PLAYLIST_STATE_STOP;
	if (playlist.random)
		randomizeOrder(0, playlist.length - 1);
	return 0;
}

static int play_order_num(int fd, int order_num, float seek_time)
{
	char path[MPD_PATH_MAX];
	enum dc_action action = seek_time ? DC_ACTION_SEEK : DC_ACTION_START;

	playlist_state = PLAYLIST_STATE_PLAY;
	assert(order_num >= 0);
	assert(seek_time >= 0);

	DEBUG("playlist: play %i:\"%s\"\n", order_num,
	      get_song_url(path, song_at(order_num)));
	dc_trigger_action(DC_ACTION_STOP, 0);
	queue_song_locked(order_num);

	ob_trigger_action(OB_ACTION_RESET);

	dc_trigger_action(action, seek_time);
	if (dc.seek_where >= 0)
		playlist.current = order_num;

	return 0;
}

int playPlaylist(int fd, int song, int stopOnError)
{
	int i = song;

	DEBUG("%s %d song(%d)\n", __func__, __LINE__, song);

	player_clearerror();

	if (song == -1) {
		if (playlist.length == 0)
			return 0;

		if (playlist_state == PLAYLIST_STATE_PLAY) {
			ob_trigger_action(OB_ACTION_PAUSE_UNSET);
			return 0;
		}
		if (playlist.current >= 0 && playlist.current < playlist.length) {
			i = playlist.current;
		} else {
			i = 0;
		}
	} else if (song < 0 || song >= playlist.length) {
		commandError(fd, ACK_ERROR_NO_EXIST,
			     "song doesn't exist: \"%i\"", song);
		return -1;
	}

	if (playlist.random) {
		if (song == -1) {
			randomizeOrder(0, playlist.length - 1);
		} else {
			if (song >= 0)
				for (i = 0; song != playlist.order[i]; i++) ;
			playlist.current = 0;
			swapOrder(i, playlist.current);
			i = playlist.current;
		}
	}

	playlist_stopOnError = stopOnError;
	playlist_errorCount = 0;

	ERROR(__FILE__ ": %d current:%d\n", __LINE__, playlist.current);
	ob_trigger_action(OB_ACTION_PAUSE_UNSET);
	return play_order_num(fd, i, 0);
}

int playPlaylistById(int fd, int id, int stopOnError)
{
	if (id == -1) {
		return playPlaylist(fd, id, stopOnError);
	}

	checkSongId(id);

	return playPlaylist(fd, playlist.idToPosition[id], stopOnError);
}

static void syncCurrentPlayerDecodeMetadata(void)
{
	Song *songPlayer = song_at(playlist.current);
	Song *song;
	int songNum;
	char path_max_tmp[MPD_PATH_MAX];

	if (!songPlayer)
		return;

	if (playlist_state != PLAYLIST_STATE_PLAY)
		return;

	songNum = playlist.order[playlist.current];
	song = playlist.songs[songNum];

	if (song->type == SONG_TYPE_URL &&
	    0 == strcmp(get_song_url(path_max_tmp, song), songPlayer->url) &&
	    !mpdTagsAreEqual(song->tag, songPlayer->tag)) {
		if (song->tag)
			freeMpdTag(song->tag);
		song->tag = mpdTagDup(songPlayer->tag);
		playlist.songMod[songNum] = playlist.version;
		incrPlaylistVersion();
	}
}

void syncPlayerAndPlaylist(void)
{
	if (playlist_state != PLAYLIST_STATE_PLAY)
		return;
	syncPlaylistWithQueue();
	/* DEBUG("queued:%d current:%d\n", playlist.queued, playlist.current); */
	if (playlist_state == PLAYLIST_STATE_PLAY &&
	    playlist.queued >= 0 &&
	    playlist.queued != playlist.current &&
	    ob_synced() &&
	    dc.state == DC_STATE_STOP &&
	    ob_get_state() != OB_STATE_PAUSE) {
		dc_trigger_action(DC_ACTION_START, 0);
	}
}

int nextSongInPlaylist(int fd)
{
	int next;
	if (playlist_state != PLAYLIST_STATE_PLAY)
		return 0;
	playlist_stopOnError = 0;
	next = next_order_num();
	if (next < 0) {
		/* we were already at last song w/o repeat: */
		incrPlaylistCurrent();
		return stopPlaylist(fd);
	}
	ob_trigger_action(OB_ACTION_PAUSE_UNSET);
	return play_order_num(fd, next, 0);
}

int getPlaylistRepeatStatus(void)
{
	return playlist.repeat;
}

int getPlaylistRandomStatus(void)
{
	return playlist.random;
}

int setPlaylistRepeatStatus(int fd, int status)
{
	if (status != 0 && status != 1) {
		commandError(fd, ACK_ERROR_ARG, "\"%i\" is not 0 or 1", status);
		return -1;
	}

	if (playlist_state == PLAYLIST_STATE_PLAY) {
		if (playlist.repeat && !status && playlist.queued == 0)
			clear_queue();
	}

	playlist.repeat = status;

	return 0;
}

int moveSongInPlaylist(int fd, int from, int to)
{
	int i;
	Song *tmpSong;
	int tmpId;
	int currentSong;

	if (from < 0 || from >= playlist.length) {
		commandError(fd, ACK_ERROR_NO_EXIST,
			     "song doesn't exist: \"%i\"", from);
		return -1;
	}

	if ((to >= 0 && to >= playlist.length) ||
	    (to < 0 && abs(to) > playlist.length)) {
		commandError(fd, ACK_ERROR_NO_EXIST,
			     "song doesn't exist: \"%i\"", to);
		return -1;
	}

	if (from == to) /* no-op */
		return 0;

	/*
	 * (to < 0) => move to offset from current song
	 * (-playlist.length == to) => move to position BEFORE current song
	 */
	currentSong = playlist.order[playlist.current];
	if (to < 0 && playlist.current >= 0) {
		if (currentSong == from)
			/* no-op, can't be moved to offset of itself */
			return 0;
		to = (currentSong + abs(to)) % playlist.length;
	}

	if (playlist_state == PLAYLIST_STATE_PLAY) {
		int queuedSong = -1;

		if (playlist.queued >= 0)
			queuedSong = playlist.order[playlist.queued];
		if (queuedSong == from || queuedSong == to
		    || currentSong == from || currentSong == to)
			clear_queue();
	}

	tmpSong = playlist.songs[from];
	tmpId = playlist.positionToId[from];
	/* move songs to one less in from->to */
	for (i = from; i < to; i++) {
		moveSongFromTo(i + 1, i);
	}
	/* move songs to one more in to->from */
	for (i = from; i > to; i--) {
		moveSongFromTo(i - 1, i);
	}
	/* put song at _to_ */
	playlist.idToPosition[tmpId] = to;
	playlist.positionToId[to] = tmpId;
	playlist.songs[to] = tmpSong;
	playlist.songMod[to] = playlist.version;
	/* now deal with order */
	if (playlist.random) {
		for (i = 0; i < playlist.length; i++) {
			if (playlist.order[i] > from && playlist.order[i] <= to) {
				playlist.order[i]--;
			} else if (playlist.order[i] < from &&
				   playlist.order[i] >= to) {
				playlist.order[i]++;
			} else if (from == playlist.order[i]) {
				playlist.order[i] = to;
			}
		}
	} else {
		if (playlist.current == from) {
			playlist.current = to;
		} else if (playlist.current > from && playlist.current <= to) {
			playlist.current--;
		} else if (playlist.current >= to && playlist.current < from) {
			playlist.current++;
		}
	}
	queueNextSongInPlaylist();
	incrPlaylistVersion();

	return 0;
}

int moveSongInPlaylistById(int fd, int id1, int to)
{
	checkSongId(id1);

	return moveSongInPlaylist(fd, playlist.idToPosition[id1], to);
}

static void orderPlaylist(void)
{
	int i;
	int queued_is_current = (playlist.queued == playlist.current);

	if (!queued_is_current &&
	    playlist_state == PLAYLIST_STATE_PLAY &&
	    playlist.queued >= 0)
		clear_queue();
	if (playlist.current >= 0 && playlist.current < playlist.length) {
		playlist.current = playlist.order[playlist.current];
		if (queued_is_current)
			playlist.queued = playlist.current;
	}
	for (i = 0; i < playlist.length; i++)
		playlist.order[i] = i;
}

static void swapOrder(int a, int b)
{
	int bak = playlist.order[a];
	playlist.order[a] = playlist.order[b];
	playlist.order[b] = bak;
}

static void randomizeOrder(int start, int end)
{
	int i;
	int ri;
	int queued_is_current = (playlist.queued == playlist.current);

	DEBUG("playlist: randomize from %i to %i\n", start, end);
	DEBUG("%s:%d current: %d\n", __func__, __LINE__, playlist.current);

	if (!queued_is_current &&
	    playlist_state == PLAYLIST_STATE_PLAY &&
	    playlist.queued >= start &&
	    playlist.queued <= end)
		clear_queue();

	for (i = start; i <= end; i++) {
		ri = random() % (end - start + 1) + start;
		if (ri == playlist.current)
			playlist.current = i;
		else if (i == playlist.current)
			playlist.current = ri;
		swapOrder(i, ri);
	}
	if (queued_is_current)
		playlist.queued = playlist.current;
	DEBUG("%s:%d current: %d\n", __func__, __LINE__, playlist.current);
}

int setPlaylistRandomStatus(int fd, int status)
{
	int statusWas = playlist.random;

	if (status != 0 && status != 1) {
		commandError(fd, ACK_ERROR_ARG, "\"%i\" is not 0 or 1", status);
		return -1;
	}

	playlist.random = status;

	if (status != statusWas) {
		if (playlist.random)
			randomizeOrder(0, playlist.length - 1);
		else
			orderPlaylist();
		if (playlist_state == PLAYLIST_STATE_PLAY) {
			queueNextSongInPlaylist();
			DEBUG("%s:%d queued: %d\n",
			      __func__,__LINE__,playlist.queued);
		}
	}

	return 0;
}

int previousSongInPlaylist(int fd)
{
	static time_t lastTime;
	time_t diff = time(NULL) - lastTime;
	int prev_order_num;

	lastTime += diff;

	if (playlist_state != PLAYLIST_STATE_PLAY)
		return 0;

	syncPlaylistWithQueue();

	if (diff && ob_get_elapsed_time() > PLAYLIST_PREV_UNLESS_ELAPSED) {
		prev_order_num = playlist.current;
	} else {
		if (playlist.current > 0)
			prev_order_num = playlist.current - 1;
		else if (playlist.repeat)
			prev_order_num = playlist.length - 1;
		else
			prev_order_num = playlist.current;
	}
	ob_trigger_action(OB_ACTION_PAUSE_UNSET);
	return play_order_num(fd, prev_order_num, 0);
}

int shufflePlaylist(int fd)
{
	int i;
	int ri;
	int playing_queued = 0;

	if (playlist.length > 1) {
		if (playlist_state == PLAYLIST_STATE_PLAY) {
			if (playlist.queued == playlist.current)
				playing_queued = 1;
			else
				clear_queue();
			/* put current playing song first */
			swapSongs(0, playlist.order[playlist.current]);
			if (playlist.random) {
				int j;
				for (j = 0; 0 != playlist.order[j]; j++) ;
				playlist.current = j;
			} else
				playlist.current = 0;
			if (playing_queued)
				playlist.queued = playlist.current;
			i = 1;
		} else {
			i = 0;
			playlist.current = -1;
		}
		/* shuffle the rest of the list */
		for (; i < playlist.length; i++) {
			ri = random() % (playlist.length - 1) + 1;
			swapSongs(i, ri);
		}
		incrPlaylistVersion();
		if (playlist_state == PLAYLIST_STATE_PLAY)
			queueNextSongInPlaylist();
	}

	return 0;
}

int deletePlaylist(int fd, char *utf8file)
{
	char path_max_tmp[MPD_PATH_MAX];

	utf8_to_fs_playlist_path(path_max_tmp, utf8file);

	if (!isPlaylist(path_max_tmp)) {
		commandError(fd, ACK_ERROR_NO_EXIST,
			     "playlist \"%s\" not found", utf8file);
		return -1;
	}

	if (unlink(path_max_tmp) < 0) {
		commandError(fd, ACK_ERROR_SYSTEM,
			     "problems deleting file");
		return -1;
	}

	return 0;
}

int savePlaylist(int fd, char *utf8file)
{
	FILE *fp;
	int i;
	struct stat sb;
	char path_max_tmp[MPD_PATH_MAX];

	if (!valid_playlist_name(fd, utf8file))
		return -1;

	utf8_to_fs_playlist_path(path_max_tmp, utf8file);
	if (!stat(path_max_tmp, &sb)) {
		commandError(fd, ACK_ERROR_EXIST, "a file or directory already "
			     "exists with the name \"%s\"", utf8file);
		return -1;
	}

	while (!(fp = fopen(path_max_tmp, "w")) && errno == EINTR);

	if (fp == NULL) {
		commandError(fd, ACK_ERROR_SYSTEM, "failed to create file");
		return -1;
	}

	for (i = 0; i < playlist.length; i++) {
		char tmp[MPD_PATH_MAX];

		get_song_url(path_max_tmp, playlist.songs[i]);
		utf8_to_fs_charset(tmp, path_max_tmp);

		if (playlist_saveAbsolutePaths &&
		    playlist.songs[i]->type == SONG_TYPE_FILE)
			fprintf(fp, "%s\n", rmp2amp_r(tmp, tmp));
		else
			fprintf(fp, "%s\n", tmp);
	}

	while (fclose(fp) && errno == EINTR) ;

	return 0;
}

int getPlaylistCurrentSong(void)
{
	DEBUG("%s:%d current: %d\n", __func__, __LINE__, playlist.current);
	if (playlist.current >= 0 && playlist.current < playlist.length) {
		return playlist.order[playlist.current];
	}

	return -1;
}

unsigned long getPlaylistVersion(void)
{
	return playlist.version;
}

int getPlaylistLength(void)
{
	return playlist.length;
}

/*
 * This command will always return 0 regardless of whether or
 * not the seek succeeded (it's always been the case, apparently)
 */
int seekSongInPlaylist(int fd, int song, float seek_time)
{
	int i = song;
	char path[MPD_PATH_MAX];

	if (song < 0 || song >= playlist.length) {
		commandError(fd, ACK_ERROR_NO_EXIST,
			     "song doesn't exist: \"%i\"", song);
		return -1;
	}

	if (playlist.random)
		for (i = 0; song != playlist.order[i]; i++) ;

	player_clearerror();
	playlist_stopOnError = 1;
	playlist_errorCount = 0;

	if (playlist_state == PLAYLIST_STATE_PLAY &&
	    (playlist.current == i && playlist.queued == i)) {
		dc_trigger_action(DC_ACTION_SEEK, seek_time);
		if (dc.seek_where != DC_SEEK_MISMATCH)
			return 0;
		/*
		 * if near end of decoding can cause seek to fail (since we're
		 * already on another song) (leading to DC_SEEK_MISMATCH),
		 * so fall through to restarting the decoder below.
		 */
	}

	DEBUG("playlist: seek %i:\"%s\"\n", i, get_song_url(path, song_at(i)));
	play_order_num(fd, i, seek_time);
	return 0;
}

int seekSongInPlaylistById(int fd, int id, float seek_time)
{
	checkSongId(id);

	return seekSongInPlaylist(fd, playlist.idToPosition[id], seek_time);
}

int getPlaylistSongId(int song)
{
	return playlist.positionToId[song];
}

int PlaylistInfo(int fd, char *utf8file, int detail)
{
	ListNode *node;
	List *list;

	if (!(list = loadStoredPlaylist(fd, utf8file)))
		return -1;

	node = list->firstNode;
	while (node != NULL) {
		char *temp = node->data;
		int wrote = 0;

		if (detail) {
			Song *song = getSongFromDB(temp);
			if (song) {
				printSongInfo(fd, song);
				wrote = 1;
			}
		}

		if (!wrote) {
			fdprintf(fd, SONG_FILE "%s\n", temp);
		}

		node = node->nextNode;
	}

	freeList(list);
	return 0;
}

int loadPlaylist(int fd, char *utf8file)
{
	ListNode *node;
	List *list;

	if (!(list = loadStoredPlaylist(fd,  utf8file)))
		return -1;

	node = list->firstNode;
	while (node != NULL) {
		char *temp = node->data;
		if ((addToPlaylist(STDERR_FILENO, temp, NULL)) < 0) {
			/* for windows compatibility, convert slashes */
			char *temp2 = xstrdup(temp);
			char *p = temp2;
			while (*p) {
				if (*p == '\\')
					*p = '/';
				p++;
			}
			if ((addToPlaylist(STDERR_FILENO, temp2, NULL)) < 0) {
				commandError(fd, ACK_ERROR_PLAYLIST_LOAD,
							"can't add file \"%s\"", temp2);
			}
			free(temp2);
		}

		node = node->nextNode;
	}

	freeList(list);
	return 0;
}

void searchForSongsInPlaylist(int fd, int numItems, LocateTagItem * items)
{
	int i;
	char **originalNeedles = xmalloc(numItems * sizeof(char *));

	for (i = 0; i < numItems; i++) {
		originalNeedles[i] = items[i].needle;
		items[i].needle = strDupToUpper(originalNeedles[i]);
	}

	for (i = 0; i < playlist.length; i++) {
		if (strstrSearchTags(playlist.songs[i], numItems, items))
			printPlaylistSongInfo(fd, i);
	}

	for (i = 0; i < numItems; i++) {
		free(items[i].needle);
		items[i].needle = originalNeedles[i];
	}

	free(originalNeedles);
}

void findSongsInPlaylist(int fd, int numItems, LocateTagItem * items)
{
	int i;

	for (i = 0; i < playlist.length; i++) {
		if (tagItemsFoundAndMatches(playlist.songs[i], numItems, items))
			printPlaylistSongInfo(fd, i);
	}
}

/*
 * Not supporting '/' was done out of laziness, and we should really
 * strive to support it in the future.
 *
 * Not supporting '\r' and '\n' is done out of protocol limitations (and
 * arguably laziness), but bending over head over heels to modify the
 * protocol (and compatibility with all clients) to support idiots who
 * put '\r' and '\n' in filenames isn't going to happen, either.
 */
int valid_playlist_name(int err_fd, const char *utf8path)
{
	if (strchr(utf8path, '/') ||
	    strchr(utf8path, '\n') ||
	    strchr(utf8path, '\r')) {
		commandError(err_fd, ACK_ERROR_ARG, "playlist name \"%s\" is "
		             "invalid: playlist names may not contain slashes,"
			     " newlines or carriage returns",
		             utf8path);
		return 0;
	}
	return 1;
}

int playlist_playing(void)
{
	return (playlist_state == PLAYLIST_STATE_PLAY);
}
