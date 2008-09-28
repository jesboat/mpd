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
#include "metadata_pipe.h"

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
static void play_order_num(int order_num, float seek_time);
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

void clearPlaylist(void)
{
	int i;

	stopPlaylist();

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
}

int clearStoredPlaylist(const char *utf8file)
{
	return removeAllFromStoredPlaylistByPath(utf8file);
}

void showPlaylist(int fd)
{
	int i;
	char path_max_tmp[MPD_PATH_MAX];

	for (i = 0; i < playlist.length; i++) {
		fdprintf(fd, "%i:%s\n", i,
		         get_song_url(path_max_tmp, playlist.songs[i]));
	}
}

void savePlaylistState(int fd)
{
	fdprintf(fd, PLAYLIST_STATE_FILE_STATE);
	switch (playlist_state) {
	case PLAYLIST_STATE_PLAY:
		switch (ob_get_state()) {
		case OB_STATE_PAUSE:
			fdprintf(fd, PLAYLIST_STATE_FILE_STATE_PAUSE "\n");
			break;
		default:
			fdprintf(fd, PLAYLIST_STATE_FILE_STATE_PLAY "\n");
		}
		fdprintf(fd, PLAYLIST_STATE_FILE_CURRENT "%i\n"
		         PLAYLIST_STATE_FILE_TIME "%lu\n",
		         playlist.order[playlist.current],
		         ob_get_elapsed_time());
		break;
	default:
		fdprintf(fd, PLAYLIST_STATE_FILE_STATE_STOP "\n");
		break;
	}
	fdprintf(fd,
		PLAYLIST_STATE_FILE_RANDOM "%i\n"
		PLAYLIST_STATE_FILE_REPEAT "%i\n"
		PLAYLIST_STATE_FILE_CROSSFADE "%i\n"
		PLAYLIST_STATE_FILE_PLAYLIST_BEGIN "\n",
		playlist.random, playlist.repeat, (int)(ob_get_xfade()));
	showPlaylist(fd);
	fdprintf(fd, PLAYLIST_STATE_FILE_PLAYLIST_END "\n");
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
		if (addToPlaylist(temp, NULL) == PLAYLIST_RESULT_SUCCESS
		    && current == song) {
			if (state == OB_STATE_PAUSE)
				ob_trigger_action(OB_ACTION_PAUSE_SET);
			if (state != OB_STATE_STOP) {
				seekSongInPlaylist(playlist.length - 1,
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
		if (!prefixcmp(buffer, PLAYLIST_STATE_FILE_STATE)) {
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
		} else if (!prefixcmp(buffer, PLAYLIST_STATE_FILE_TIME)) {
			seek_time =
			    atoi(&(buffer[strlen(PLAYLIST_STATE_FILE_TIME)]));
		} else
		    if (!prefixcmp(buffer, PLAYLIST_STATE_FILE_REPEAT)) {
			if (strcmp
			    (&(buffer[strlen(PLAYLIST_STATE_FILE_REPEAT)]),
			     "1") == 0) {
				setPlaylistRepeatStatus(1);
			} else
				setPlaylistRepeatStatus(0);
		} else if (!prefixcmp(buffer, PLAYLIST_STATE_FILE_CROSSFADE)) {
			ob_set_xfade(atoi(&(buffer
					     [strlen
					      (PLAYLIST_STATE_FILE_CROSSFADE)])));
		} else if (!prefixcmp(buffer, PLAYLIST_STATE_FILE_RANDOM)) {
			if (strcmp
			    (&
			     (buffer
			      [strlen(PLAYLIST_STATE_FILE_RANDOM)]),
			     "1") == 0) {
				setPlaylistRandomStatus(1);
			} else
				setPlaylistRandomStatus(0);
		} else if (!prefixcmp(buffer, PLAYLIST_STATE_FILE_CURRENT)) {
			if (strlen(buffer) ==
			    strlen(PLAYLIST_STATE_FILE_CURRENT))
				state_file_fatal();
			current = atoi(&(buffer
					 [strlen
					  (PLAYLIST_STATE_FILE_CURRENT)]));
		} else if (!prefixcmp(buffer,
		                      PLAYLIST_STATE_FILE_PLAYLIST_BEGIN)) {
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

enum playlist_result playlistInfo(int fd, int song)
{
	int i;
	int begin = 0;
	int end = playlist.length;

	if (song >= 0) {
		begin = song;
		end = song + 1;
	}
	if (song >= playlist.length)
		return PLAYLIST_RESULT_BAD_RANGE;

	for (i = begin; i < end; i++)
		printPlaylistSongInfo(fd, i);

	return PLAYLIST_RESULT_SUCCESS;
}

static int song_id_exists(int id)
{
	return id >= 0 && id < PLAYLIST_HASH_MULT*playlist_max_length &&
		playlist.idToPosition[id] != -1;
}

enum playlist_result playlistId(int fd, int id)
{
	int i;
	int begin = 0;
	int end = playlist.length;

	if (id >= 0) {
		if (!song_id_exists(id))
			return PLAYLIST_RESULT_NO_SUCH_SONG;

		begin = playlist.idToPosition[id];
		end = begin + 1;
	}

	for (i = begin; i < end; i++)
		printPlaylistSongInfo(fd, i);

	return PLAYLIST_RESULT_SUCCESS;
}

static void swapSongs(int song1, int song2)
{
	Song *sTemp;
	int iTemp;

	assert(song1 < playlist.length);
	assert(song1 >= 0);
	assert(song2 < playlist.length);
	assert(song2 >= 0);

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
		if (playlist.length > 0) {
			if (playlist.random)
				randomizeOrder(0, playlist.length - 1);
			else
				playlist.current = -1;
		}
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

	if (player_errno != PLAYER_ERROR_NONE) {
		DEBUG("playlist: error: %s\n", player_strerror());
		playlist.current = playlist.queued;
		player_clearerror();
	} else if (playlist.queued >= 0 &&
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

enum playlist_result addToPlaylist(const char *url, int *added_id)
{
	Song *song;

	DEBUG("add to playlist: %s\n", url);

	if ((song = getSongFromDB(url))) {
	} else if (!(isValidRemoteUtf8Url(url) &&
		     (song = newSong(url, SONG_TYPE_URL, NULL)))) {
		return PLAYLIST_RESULT_NO_SUCH_SONG;
	}

	return addSongToPlaylist(song, added_id);
}

int addToStoredPlaylist(const char *url, const char *utf8file)
{
	Song *song;

	DEBUG("add to stored playlist: %s\n", url);

	song = getSongFromDB(url);
	if (song)
		return appendSongToStoredPlaylistByPath(utf8file, song);

	if (!isValidRemoteUtf8Url(url))
		return ACK_ERROR_NO_EXIST;

	song = newSong(url, SONG_TYPE_URL, NULL);
	if (song) {
		int ret = appendSongToStoredPlaylistByPath(utf8file, song);
		freeJustSong(song);
		return ret;
	}

	return ACK_ERROR_NO_EXIST;
}

enum playlist_result addSongToPlaylist(Song * song, int *added_id)
{
	int id;

	if (playlist.length == playlist_max_length)
		return PLAYLIST_RESULT_TOO_LARGE;

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

	/* we could be playing (but done decoding) the last song */
	if (ob_get_state() != OB_STATE_STOP) {
		playlist_state = PLAYLIST_STATE_PLAY;
		queueNextSongInPlaylist();
	}

	if (added_id)
		*added_id = id;

	return PLAYLIST_RESULT_SUCCESS;
}

enum playlist_result swapSongsInPlaylist(int song1, int song2)
{
	int queuedSong = -1;
	int currentSong;
	int queued_is_current = (playlist.queued == playlist.current);

	if (song1 < 0 || song1 >= playlist.length ||
	    song2 < 0 || song2 >= playlist.length)
		return PLAYLIST_RESULT_BAD_RANGE;

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
		if (queued_is_current)
			playlist.queued = playlist.current;
	}

	queueNextSongInPlaylist();
	incrPlaylistVersion();

	return PLAYLIST_RESULT_SUCCESS;
}

enum playlist_result swapSongsInPlaylistById(int id1, int id2)
{
	if (!song_id_exists(id1) || !song_id_exists(id2))
		return PLAYLIST_RESULT_NO_SUCH_SONG;

	return swapSongsInPlaylist(playlist.idToPosition[id1],
				   playlist.idToPosition[id2]);
}

#define moveSongFromTo(from, to) { \
	playlist.idToPosition[playlist.positionToId[from]] = to; \
	playlist.positionToId[to] = playlist.positionToId[from]; \
	playlist.songs[to] = playlist.songs[from]; \
	playlist.songMod[to] = playlist.version; \
}

enum playlist_result deleteFromPlaylist(int song)
{
	int i;
	int songOrder;
	int stop_current = 0;
	int prev_queued = playlist.queued;

	if (song < 0 || song >= playlist.length)
		return PLAYLIST_RESULT_BAD_RANGE;

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
		if (playlist.current == prev_queued)
			playlist.queued = playlist.current - 1;
		playlist.current--;
	} else if (playlist.current >= playlist.length) {
		incrPlaylistCurrent();
	}
	if (stop_current) {
		/* DEBUG(__FILE__": %d\n", __LINE__); */
		if (playlist.current >= 0 && songOrder > 0)
			play_order_num(playlist.current, 0);
		else
			stopPlaylist();
	} else {
		/* DEBUG(__FILE__": %d\n", __LINE__); */
		queueNextSongInPlaylist();
	}

	return PLAYLIST_RESULT_SUCCESS;
}

enum playlist_result deleteFromPlaylistById(int id)
{
	if (!song_id_exists(id))
		return PLAYLIST_RESULT_NO_SUCH_SONG;

	return deleteFromPlaylist(playlist.idToPosition[id]);
}

void deleteASongFromPlaylist(Song * song)
{
	int i;

	if (NULL == playlist.songs)
		return;

	for (i = 0; i < playlist.length; i++) {
		if (song == playlist.songs[i]) {
			deleteFromPlaylist(i);
		}
	}
}

void stopPlaylist(void)
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
}

static void play_order_num(int order_num, float seek_time)
{
	char path[MPD_PATH_MAX];
	enum dc_action action = seek_time ? DC_ACTION_SEEK : DC_ACTION_START;

	playlist_state = PLAYLIST_STATE_PLAY;
	assert(order_num >= 0);
	assert(seek_time >= 0);
	assert(song_at(order_num));

	DEBUG("playlist: play %i:\"%s\"\n", order_num,
	      get_song_url(path, song_at(order_num)));
	dc_trigger_action(DC_ACTION_STOP, 0);
	queue_song_locked(order_num);

	ob_trigger_action(OB_ACTION_RESET);

	dc_trigger_action(action, seek_time);
	if (dc.seek_where >= 0)
		playlist.current = order_num;
}

enum playlist_result playPlaylist(int song, int stopOnError)
{
	int i = song;

	DEBUG("%s %d song(%d)\n", __func__, __LINE__, song);

	player_clearerror();

	if (song == -1) {
		if (playlist.length == 0)
			return PLAYLIST_RESULT_SUCCESS;

		if (playlist_state == PLAYLIST_STATE_PLAY) {
			ob_trigger_action(OB_ACTION_PAUSE_UNSET);
			return PLAYLIST_RESULT_SUCCESS;
		}
		if (playlist.current >= 0 && playlist.current < playlist.length) {
			i = playlist.current;
		} else {
			i = 0;
		}
	} else if (song < 0 || song >= playlist.length) {
		return PLAYLIST_RESULT_BAD_RANGE;
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
			randomizeOrder(i + 1, playlist.length - 1);
		}
	}

	playlist_stopOnError = stopOnError;
	playlist_errorCount = 0;

	ERROR(__FILE__ ": %d current:%d\n", __LINE__, playlist.current);
	ob_trigger_action(OB_ACTION_PAUSE_UNSET);
	play_order_num(i, 0);
	return PLAYLIST_RESULT_SUCCESS;
}

enum playlist_result playPlaylistById(int id, int stopOnError)
{
	if (id == -1) {
		return playPlaylist(id, stopOnError);
	}

	if (!song_id_exists(id))
		return PLAYLIST_RESULT_NO_SUCH_SONG;

	return playPlaylist(playlist.idToPosition[id], stopOnError);
}

/* This is used when we stream data out to shout while playing static files */
struct mpd_tag *playlist_current_tag(void)
{
	Song *song = song_at(playlist.current);

	/* Non-file song tags can get swept out from under us */
	return (song && song->type == SONG_TYPE_FILE) ? song->tag : NULL;
}

/* This receives dynamic metadata updates from streams */
static void sync_metadata(void)
{
	Song *song;
	struct mpd_tag *tag;

	if (!(tag = metadata_pipe_current()))
		return;
	song = song_at(playlist.current);
	if (!song || song->type != SONG_TYPE_URL ||
	    tag_equal(song->tag, tag)) {
		tag_free(tag);
		return;
	}
	if (song->tag)
		tag_free(song->tag);
	song->tag = tag;
	playlist.songMod[playlist.order[playlist.current]] = playlist.version;
	incrPlaylistVersion();
}

void syncPlayerAndPlaylist(void)
{
	if (playlist_state != PLAYLIST_STATE_PLAY)
		return;
	syncPlaylistWithQueue();
	sync_metadata();
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

void nextSongInPlaylist(void)
{
	int next;
	if (playlist_state != PLAYLIST_STATE_PLAY)
		return;
	playlist_stopOnError = 0;
	next = next_order_num();
	if (next < 0) {
		/* we were already at last song w/o repeat: */
		incrPlaylistCurrent();
		stopPlaylist();
		return;
	}
	ob_trigger_action(OB_ACTION_PAUSE_UNSET);
	play_order_num(next, 0);
}

int getPlaylistRepeatStatus(void)
{
	return playlist.repeat;
}

int getPlaylistRandomStatus(void)
{
	return playlist.random;
}

void setPlaylistRepeatStatus(int status)
{
	if (playlist_state == PLAYLIST_STATE_PLAY) {
		if (playlist.repeat && !status && playlist.queued == 0)
			clear_queue();
	}

	playlist.repeat = status;
}

enum playlist_result moveSongInPlaylist(int from, int to)
{
	int i;
	Song *tmpSong;
	int tmpId;
	int currentSong;
	int queued_is_current = (playlist.queued == playlist.current);

	if (from < 0 || from >= playlist.length)
		return PLAYLIST_RESULT_BAD_RANGE;

	if ((to >= 0 && to >= playlist.length) ||
	    (to < 0 && abs(to) > playlist.length))
		return PLAYLIST_RESULT_BAD_RANGE;

	if (from == to) /* no-op */
		return PLAYLIST_RESULT_SUCCESS;

	/*
	 * (to < 0) => move to offset from current song
	 * (-playlist.length == to) => move to position BEFORE current song
	 */
	currentSong = playlist.order[playlist.current];
	if (to < 0 && playlist.current >= 0) {
		if (currentSong == from)
			/* no-op, can't be moved to offset of itself */
			return PLAYLIST_RESULT_SUCCESS;
		to = (currentSong + abs(to)) % playlist.length;
	}

	if (playlist_state == PLAYLIST_STATE_PLAY && !queued_is_current) {
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
		if (queued_is_current)
			playlist.queued = playlist.current;
	}
	queueNextSongInPlaylist();
	incrPlaylistVersion();

	return PLAYLIST_RESULT_SUCCESS;
}

enum playlist_result moveSongInPlaylistById(int id1, int to)
{
	if (!song_id_exists(id1))
		return PLAYLIST_RESULT_NO_SUCH_SONG;

	return moveSongInPlaylist(playlist.idToPosition[id1], to);
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
	int bak;

	assert(a < playlist.length);
	assert(a >= 0);
	assert(b < playlist.length);
	assert(b >= 0);

	bak = playlist.order[a];
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

	for (i = start; i < end; i++) {
		ri = random() % (end - i) + start;
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

void setPlaylistRandomStatus(int status)
{
	int statusWas = playlist.random;

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
}

void previousSongInPlaylist(void)
{
	static time_t lastTime;
	time_t diff = time(NULL) - lastTime;
	int prev_order_num;

	lastTime += diff;

	if (playlist_state != PLAYLIST_STATE_PLAY)
		return;

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
	play_order_num(prev_order_num, 0);
}

void shufflePlaylist(void)
{
	int i;
	int ri;
	int playing_queued = 0;
	int length = playlist.length;

	if (length > 1) {
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
		for (; --length > 0; ++i) {
			ri = random() % length + 1;
			swapSongs(i, ri);
		}
		incrPlaylistVersion();
		if (playlist_state == PLAYLIST_STATE_PLAY)
			queueNextSongInPlaylist();
	}
}

enum playlist_result deletePlaylist(const char *utf8file)
{
	char path_max_tmp[MPD_PATH_MAX];

	utf8_to_fs_playlist_path(path_max_tmp, utf8file);

	if (!isPlaylist(path_max_tmp))
		return PLAYLIST_RESULT_NO_SUCH_LIST;

	if (unlink(path_max_tmp) < 0)
		return PLAYLIST_RESULT_ERRNO;

	return PLAYLIST_RESULT_SUCCESS;
}

enum playlist_result savePlaylist(const char *utf8file)
{
	FILE *fp;
	int i;
	struct stat sb;
	char path_max_tmp[MPD_PATH_MAX];

	if (!is_valid_playlist_name(utf8file))
		return PLAYLIST_RESULT_BAD_NAME;

	utf8_to_fs_playlist_path(path_max_tmp, utf8file);
	if (!stat(path_max_tmp, &sb))
		return PLAYLIST_RESULT_LIST_EXISTS;

	while (!(fp = fopen(path_max_tmp, "w")) && errno == EINTR);

	if (fp == NULL)
		return PLAYLIST_RESULT_ERRNO;

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

	return PLAYLIST_RESULT_SUCCESS;
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
enum playlist_result seekSongInPlaylist(int song, float seek_time)
{
	int i = song;
	char path[MPD_PATH_MAX];

	if (song < 0 || song >= playlist.length)
		return PLAYLIST_RESULT_BAD_RANGE;

	if (playlist.random)
		for (i = 0; song != playlist.order[i]; i++) ;

	player_clearerror();
	playlist_stopOnError = 1;
	playlist_errorCount = 0;

	if (playlist_state == PLAYLIST_STATE_PLAY &&
	    (playlist.current == i && playlist.queued == i)) {
		dc_trigger_action(DC_ACTION_SEEK, seek_time);
		if (dc.seek_where != DC_SEEK_MISMATCH)
			return PLAYLIST_RESULT_SUCCESS;
		/*
		 * if near end of decoding can cause seek to fail (since we're
		 * already on another song) (leading to DC_SEEK_MISMATCH),
		 * so fall through to restarting the decoder below.
		 */
	}

	DEBUG("playlist: seek %i:\"%s\"\n", i, get_song_url(path, song_at(i)));
	play_order_num(i, seek_time);
	return PLAYLIST_RESULT_SUCCESS;
}

enum playlist_result seekSongInPlaylistById(int id, float seek_time)
{
	if (!song_id_exists(id))
		return PLAYLIST_RESULT_NO_SUCH_SONG;

	return seekSongInPlaylist(playlist.idToPosition[id], seek_time);
}

int getPlaylistSongId(int song)
{
	return playlist.positionToId[song];
}

int PlaylistInfo(int fd, const char *utf8file, int detail)
{
	ListNode *node;
	List *list;

	if (!(list = loadStoredPlaylist(utf8file)))
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

enum playlist_result loadPlaylist(int fd, const char *utf8file)
{
	ListNode *node;
	List *list;

	if (!(list = loadStoredPlaylist(utf8file)))
		return PLAYLIST_RESULT_NO_SUCH_LIST;

	node = list->firstNode;
	while (node != NULL) {
		char *temp = node->data;
		if ((addToPlaylist(temp, NULL)) != PLAYLIST_RESULT_SUCCESS) {
			/* for windows compatibility, convert slashes */
			char *temp2 = xstrdup(temp);
			char *p = temp2;
			while (*p) {
				if (*p == '\\')
					*p = '/';
				p++;
			}
			if ((addToPlaylist(temp, NULL)) != PLAYLIST_RESULT_SUCCESS) {
				commandError(fd, ACK_ERROR_PLAYLIST_LOAD,
							"can't add file \"%s\"", temp2);
			}
			free(temp2);
		}

		node = node->nextNode;
	}

	freeList(list);
	return PLAYLIST_RESULT_SUCCESS;
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
int is_valid_playlist_name(const char *utf8path)
{
	return strchr(utf8path, '/') == NULL &&
		strchr(utf8path, '\n') == NULL &&
		strchr(utf8path, '\r') == NULL;
}

int playlist_playing(void)
{
	return (playlist_state == PLAYLIST_STATE_PLAY);
}
