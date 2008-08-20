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

#include "decode.h"
#include "outputBuffer.h"
#include "player_error.h"
#include "playlist.h"
#include "pcm_utils.h"
#include "path.h"
#include "log.h"
#include "ls.h"
#include "condition.h"

static struct condition dc_action_cond = STATIC_COND_INITIALIZER;
static struct condition dc_halt_cond = STATIC_COND_INITIALIZER;

struct decoder_control dc; /* ugh, global for now... */

/* blocking, waits until the signaled thread has replied */
void dc_trigger_action(enum dc_action action, float seek_where)
{
	assert(!pthread_equal(pthread_self(), dc.thread));
	/* assert(pthread_equal(pthread_self(), main_thread)); */
	assert(action != DC_ACTION_NONE);

	/* DEBUG(__FILE__ ":%s %d\n", __func__,__LINE__); */
	cond_enter(&dc_action_cond);
	assert(dc.action == DC_ACTION_NONE);
	if (action == DC_ACTION_SEEK)
		dc.seek_where = seek_where; /* usually 0 */
	dc.action = action;
	do {
		/* DEBUG(__FILE__ ":%s %d\n", __func__,__LINE__); */
		cond_signal(&dc_halt_cond); /* blind signal w/o lock */
		/* DEBUG(__FILE__ ":%s %d\n", __func__,__LINE__); */
		cond_timedwait(&dc_action_cond, 10);
		/* DEBUG(__FILE__ ":%s %d\n", __func__,__LINE__); */
	} while (dc.action != DC_ACTION_NONE); /* spurious wakeup protection */
	/* DEBUG(__FILE__ ":%s %d\n", __func__,__LINE__); */
	cond_leave(&dc_action_cond);
	/* DEBUG(__FILE__ ":%s %d\n", __func__,__LINE__); */
}

static void take_action(void)
{
	assert(pthread_equal(pthread_self(), dc.thread));
	assert(dc.state == DC_STATE_STOP);
	/* DEBUG("%s dc.action(%d): %d\n", __func__,__LINE__, dc.action); */
	cond_enter(&dc_action_cond);
	/* DEBUG("%s dc.action(%d): %d\n", __func__,__LINE__, dc.action); */

	switch (dc.action) {
	case DC_ACTION_NONE: goto out;
	case DC_ACTION_START:
	case DC_ACTION_SEEK:
		dc.state = DC_STATE_DECODE;
		return;
	case DC_ACTION_STOP: dc.state = DC_STATE_STOP; break;
	case DC_ACTION_QUIT: dc.state = DC_STATE_QUIT;
	}
	dc.action = DC_ACTION_NONE;
	cond_signal(&dc_action_cond);
out:
	assert(dc.action == DC_ACTION_NONE);
	cond_leave(&dc_action_cond);
}

/*
 * This will grab an action, but will not signal the calling thread.
 * dc_action_end() is required to signal the calling thread
 */
void dc_action_begin(void)
{
	enum dc_action ret = dc.action;

	assert(pthread_equal(pthread_self(), dc.thread));

	if (ret != DC_ACTION_NONE) {
		/* DEBUG(__FILE__ ":%s %d\n", __func__,__LINE__); */
		cond_enter(&dc_action_cond);
		/* dc.action can't get set to NONE outside this thread */
		assert(dc.action == ret);
		if (ret == DC_ACTION_SEEK)
			ob_seek_start();
	}
}

void dc_action_end(void)
{
	assert(pthread_equal(pthread_self(), dc.thread));
	assert(dc.action != DC_ACTION_NONE);
	/* DEBUG("DONE ACTION %d\n", dc.action); */
	if (dc.action == DC_ACTION_SEEK)
		ob_seek_finish();
	dc.action = DC_ACTION_NONE;

	cond_signal(&dc_action_cond);
	cond_leave(&dc_action_cond);
}

void dc_action_seek_fail(enum seek_err_type err_type)
{
	assert(pthread_equal(pthread_self(), dc.thread));
	cond_enter(&dc_action_cond);
	assert(dc.action == DC_ACTION_SEEK);
	dc.action = DC_ACTION_NONE;
	dc.seek_where = err_type;
	cond_signal(&dc_action_cond);
	cond_leave(&dc_action_cond);
}

/* Returns true if we need to interrupt the decoding inside an inputPlugin */
int dc_intr(void)
{
	if (!pthread_equal(pthread_self(), dc.thread))
		return 0;
	switch (dc.action) {
	case DC_ACTION_NONE:
	case DC_ACTION_SEEK:
		return 0;
	default:
		/* DEBUG(__FILE__": %s %d\n", __func__, __LINE__); */
		/* DEBUG("dc.action: %d\n", (int)dc.action); */
		return 1;
	}
}

int dc_seek(void)
{
	if (pthread_equal(pthread_self(), dc.thread))
		return (dc.action == DC_ACTION_SEEK);
	return 0;
}

static void finalize_per_track_actions(void)
{
	enum dc_action action;
	/* DEBUG(":%s dc.action(%d): %d\n", __func__,__LINE__, dc.action); */
	assert(pthread_equal(pthread_self(), dc.thread));
	cond_enter(&dc_action_cond);
	dc.state = DC_STATE_STOP;
	action = dc.action;
	dc.action = DC_ACTION_NONE;

	if (action == DC_ACTION_STOP) {
		cond_signal(&dc_action_cond);
	} else if (action == DC_ACTION_SEEK) {
		dc.seek_where = DC_SEEK_MISMATCH;
		cond_signal(&dc_action_cond);
	}
	cond_leave(&dc_action_cond);
	/* DEBUG(":%s dc.action(%d): %d\n", __func__,__LINE__, dc.action); */
}

static void decode_start(void)
{
	int err = -1;
	int close_instream = 1;
	InputStream is;
	InputPlugin *plugin = NULL;
	char path_max_fs[MPD_PATH_MAX];
	char path_max_utf8[MPD_PATH_MAX];
	assert(pthread_equal(pthread_self(), dc.thread));
	assert(dc.state == DC_STATE_DECODE);
	assert(dc.current_song);
	get_song_url(path_max_utf8, dc.current_song);
	assert(*path_max_utf8);

	switch (dc.action) {
	case DC_ACTION_START:
		dc_action_end();
		break;
	case DC_ACTION_SEEK:
		/* DEBUG("dc.seek_where(%d): %f\n", __LINE__, dc.seek_where); */
		/* make sure dc_action_start() works inside inputPlugins: */
		cond_leave(&dc_action_cond);
		/* DEBUG("dc.action(%d) %d\n", __LINE__, dc.action); */
		break;
	default: assert("unknown action!" && 0);
	}

	if (isRemoteUrl(path_max_utf8)) {
		pathcpy_trunc(path_max_fs, path_max_utf8);
	} else {
		rmp2amp_r(path_max_fs,
		          utf8_to_fs_charset(path_max_fs, path_max_utf8));
	}

	if (openInputStream(&is, path_max_fs) < 0) {
		DEBUG("couldn't open song: %s\n", path_max_fs);
		player_seterror(PLAYER_ERROR_FILENOTFOUND, dc.current_song);
		return;
	}

	if (isRemoteUrl(path_max_utf8)) {
		unsigned int next = 0;

		/* first we try mime types: */
		while (err && (plugin = getInputPluginFromMimeType(is.mime, next++))) {
			if (!plugin->streamDecodeFunc)
				continue;
			if (!(plugin->streamTypes & INPUT_PLUGIN_STREAM_URL))
				continue;
			if (plugin->tryDecodeFunc
			    && !plugin->tryDecodeFunc(&is))
				continue;
			err = plugin->streamDecodeFunc(&is);
			break;
		}

		/* if that fails, try suffix matching the URL: */
		if (plugin == NULL) {
			const char *s = getSuffix(path_max_utf8);
			next = 0;
			while (err && (plugin = getInputPluginFromSuffix(s, next++))) {
				if (!plugin->streamDecodeFunc)
					continue;
				if (!(plugin->streamTypes &
				      INPUT_PLUGIN_STREAM_URL))
					continue;
				if (plugin->tryDecodeFunc &&
				    !plugin->tryDecodeFunc(&is))
					continue;
				err = plugin->streamDecodeFunc(&is);
				break;
			}
		}
		/* fallback to mp3: */
		/* this is needed for bastard streams that don't have a suffix
		   or set the mimeType */
		if (plugin == NULL) {
			/* we already know our mp3Plugin supports streams, no
			 * need to check for stream{Types,DecodeFunc} */
			if ((plugin = getInputPluginFromName("mp3")))
				err = plugin->streamDecodeFunc(&is);
		}
	} else {
		unsigned int next = 0;
		const char *s = getSuffix(path_max_utf8);
		while (err && (plugin = getInputPluginFromSuffix(s, next++))) {
			if (!plugin->streamTypes & INPUT_PLUGIN_STREAM_FILE)
				continue;

			if (plugin->tryDecodeFunc &&
			    !plugin->tryDecodeFunc(&is))
				continue;

			if (plugin->fileDecodeFunc) {
				closeInputStream(&is);
				close_instream = 0;
				err = plugin->fileDecodeFunc(path_max_fs);
				break;
			} else if (plugin->streamDecodeFunc) {
				err = plugin->streamDecodeFunc(&is);
				break;
			}
		}
	}

	if (err) {
		if (plugin)
			player_seterror(PLAYER_ERROR_SYSTEM, dc.current_song);
		else
			player_seterror(PLAYER_ERROR_UNKTYPE, dc.current_song);
	}
	if (player_errno)
		ERROR("player_error: %s\n", player_strerror());
	if (close_instream)
		closeInputStream(&is);
}

static void * decoder_task(mpd_unused void *arg)
{
	assert(pthread_equal(pthread_self(), dc.thread));
	cond_enter(&dc_halt_cond);
	while (1) {
		take_action();
		switch (dc.state) {
		case DC_STATE_STOP:
			/* DEBUG(__FILE__": halted %d\n", __LINE__); */
			cond_wait(&dc_halt_cond);
			/* DEBUG(__FILE__": unhalted %d\n", __LINE__); */
			break;
		case DC_STATE_DECODE:
			/* DEBUG(__FILE__": %s %d\n", __func__, __LINE__); */
			/* DEBUG("dc.action: %d\n", (int)dc.action); */
			if ((dc.current_song = playlist_queued_song())) {
				char p[MPD_PATH_MAX];
				ob_advance_sequence();
				get_song_url(p, dc.current_song);
				DEBUG("decoding song: %s\n", p);
				decode_start();
				DEBUG("DONE decoding song: %s\n", p);
				ob_flush();
				dc.current_song = NULL;
			}
			finalize_per_track_actions();
			playlist_queue_next();
			break;
		case DC_STATE_QUIT:
			goto out;
		}
	}
out:
	cond_leave(&dc_halt_cond);
	assert(dc.state == DC_STATE_QUIT);
	return NULL;
}

void decoder_init(void)
{
	pthread_attr_t attr;
	assert(!dc.thread);

	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	if (pthread_create(&dc.thread, &attr, decoder_task, NULL))
		FATAL("Failed to spawn decoder task: %s\n", strerror(errno));
}

int dc_try_unhalt(void)
{
	assert(!pthread_equal(pthread_self(), dc.thread));
	return cond_signal_trysync(&dc_halt_cond);
}

void dc_halt(void)
{
	assert(pthread_equal(pthread_self(), dc.thread));
	cond_wait(&dc_halt_cond);
}
