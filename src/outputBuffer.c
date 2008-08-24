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

#include "outputBuffer.h"

#include "utils.h"
#include "normalize.h"
#include "ringbuf.h"
#include "condition.h"
#include "song.h"
#include "main_notify.h"
#include "player_error.h"
#include "log.h"
#include "action_status.h"

/* typically have 2048-4096 of these structs, so pack tightly */
struct ob_chunk {
	mpd_uint16 len; /* 0: skip this chunk */
	mpd_uint16 bit_rate;
	float time;
	mpd_uint8 seq; /* see seq_ok() for explanation */
	char data[CHUNK_SIZE];
};

static struct ob_chunk silence;

enum ob_xfade_state {
	XFADE_DISABLED = 0,
	XFADE_ENABLED
};

static struct condition ob_action_cond = STATIC_COND_INITIALIZER;
static struct condition ob_halt_cond = STATIC_COND_INITIALIZER;
static struct condition ob_seq_cond = STATIC_COND_INITIALIZER;

struct output_buffer {
	struct ringbuf *index; /* index for chunks */
	struct ob_chunk *chunks;

	size_t bpp_max; /* buffer_before_play, user setting, in chunks */
	size_t bpp_cur; /* current prebuffer size (in chunks) */

	enum ob_state state; /* protected by ob_action_cond */
	enum ob_action action; /* protected by ob_action_cond */
	enum ob_xfade_state xfade_state; /* thread-internal */
	int sw_vol;
	int bit_rate;
	float total_time;
	float elapsed_time;
	AudioFormat audio_format;
	size_t xfade_cur;
	size_t xfade_max;
	float xfade_time;
	void *conv_buf;
	size_t conv_buf_len;
	pthread_t thread;
	ConvState conv_state;
	unsigned int seq_drop;
	unsigned int seq_player; /* only gets changed by ob.thread */
	mpd_uint8 seq_decoder; /* only gets changed by dc.thread */
	struct ringbuf preseek_index;
	enum ob_state preseek_state;
	mpd_uint16 *preseek_len;
};

static struct output_buffer ob;

#include "outputBuffer_xfade.h"
#include "outputBuffer_accessors.h"

static enum action_status ob_do_stop(void);
static void stop_playback(void)
{
	assert(pthread_equal(pthread_self(), ob.thread));
	cond_enter(&ob_action_cond);
	ob_do_stop();
	cond_leave(&ob_action_cond);
}

void ob_trigger_action(enum ob_action action)
{
	/*
	 * This can be called by both dc.thread and main_thread, but only one
	 * action can be in progress at once.  So we use this private mutex
	 * to protect against simultaneous invocations stepping over
	 * each other
	 */
	static pthread_mutex_t trigger_lock = PTHREAD_MUTEX_INITIALIZER;
	pthread_mutex_lock(&trigger_lock);
	DEBUG(__FILE__": %d action: %d\n", __LINE__, action);
	assert(!pthread_equal(pthread_self(), ob.thread));
	DEBUG(__FILE__":%s %d\n", __func__, __LINE__);
	cond_enter(&ob_action_cond);
	DEBUG(__FILE__":%s %d\n", __func__, __LINE__);
	assert(ob.action == OB_ACTION_NONE);

	if (pthread_equal(pthread_self(), dc.thread))
		assert(action == OB_ACTION_PLAY ||
		       action == OB_ACTION_SEEK_START ||
		       action == OB_ACTION_SEEK_FINISH);
	else
		assert(action != OB_ACTION_PLAY &&
		       action != OB_ACTION_SEEK_START &&
		       action != OB_ACTION_SEEK_FINISH);
	ob.action = action;
	do {
		switch (ob.state) {
		case OB_STATE_PAUSE:
		case OB_STATE_STOP:
		case OB_STATE_SEEK:
			DEBUG(__FILE__":%s %d\n", __func__, __LINE__);
			cond_signal_sync(&ob_halt_cond);
			DEBUG(__FILE__":%s %d\n", __func__, __LINE__);
			break;
		default: break;
		}
		DEBUG(__FILE__":%s %d\n", __func__, __LINE__);
		cond_wait(&ob_action_cond);
	} while (ob.action != OB_ACTION_NONE);
	DEBUG(__FILE__":%s %d\n", __func__, __LINE__);
	assert(ob.action == OB_ACTION_NONE);
	cond_leave(&ob_action_cond);
	DEBUG(__FILE__":%s %d\n", __func__, __LINE__);
	pthread_mutex_unlock(&trigger_lock);
}

static enum action_status ob_finalize_action(void)
{
	assert(pthread_equal(pthread_self(), ob.thread));
	ob.action = OB_ACTION_NONE;
	/* DEBUG(__FILE__ ":%s signaling %d\n", __func__, __LINE__); */
	cond_signal(&ob_action_cond);
	cond_leave(&ob_action_cond);
	return AS_COMPLETE;
}

/* marks all buffered chunks with sequence number matching `seq' as invalid */
static enum action_status ob_do_drop(void)
{
	struct iovec vec[2];
	long i;
	unsigned int seq_drop;

	cond_enter(&ob_seq_cond);
	seq_drop = ob.seq_drop;
	/* drop the audio that we've already pushed to the device, too */
	if (seq_drop == ob.seq_player)
		dropBufferedAudio();
	cond_leave(&ob_seq_cond);

	assert(pthread_equal(pthread_self(), ob.thread));
	for (i = (long)ringbuf_get_read_vector(ob.index, vec); --i >= 0; ) {
		struct ob_chunk *c = get_chunk(vec, i);
		assert(c);
		if (c->seq == seq_drop)
			c->len = 0;
	}
	return ob_finalize_action();
}

static enum action_status ob_do_pause(void)
{
	assert(pthread_equal(pthread_self(), ob.thread));
	ob.xfade_state = XFADE_DISABLED;
	/*
	 * This will eventually set certain outputs (like shout) into 'pause'
	 * state where it'll just play silence instead of disconnecting
	 * listeners
	 */
	dropBufferedAudio();
	closeAudioDevice();
	ob.state = OB_STATE_PAUSE;
	return AS_INPROGRESS;
}

static void reader_reset_buffer(void)
{
	struct iovec vec[2];
	size_t nr;
	long i;

	assert(pthread_equal(pthread_self(), ob.thread));
	nr = ringbuf_get_read_vector(ob.index, vec);
	for (i = nr; --i >= 0; ) {
		struct ob_chunk *c = get_chunk(vec, i);
		assert(c);
		c->len = 0;
	}
	ringbuf_read_advance(ob.index, nr);
}

static void ob_seq_player_set(unsigned int seq_num)
{
	cond_enter(&ob_seq_cond);
	ob.seq_player = seq_num;
	cond_signal(&ob_seq_cond);
	cond_leave(&ob_seq_cond);
}

static enum action_status ob_do_reset(int close)
{
	assert(pthread_equal(pthread_self(), ob.thread));
	ob.elapsed_time = 0;
	ob.total_time = 0;
	reader_reset_buffer();
	dropBufferedAudio();
	if (close)
		closeAudioDevice();
	ob.xfade_state = XFADE_DISABLED;
	ob_seq_player_set((unsigned int)ob.seq_decoder);
	return ob_finalize_action();
}

static enum action_status ob_do_stop(void)
{
	assert(pthread_equal(pthread_self(), ob.thread));
	if (ob.state == OB_STATE_STOP)
		return AS_INPROGRESS;
	ob.state = OB_STATE_STOP;
	return ob_do_reset(1);
}

/*
 * we need to reset the buffer *before* we seek because the decoder
 * _may_ try to flush out the last remnants of the previously decoded audio,
 * so we need to ensure there is space available for that
 */
static enum action_status ob_do_seek_start(void)
{
	int i;

	assert(pthread_equal(pthread_self(), ob.thread));

	/* preserve pre-seek ringbuf and state information */
	memcpy(&ob.preseek_index, ob.index, sizeof(struct ringbuf));
	for (i = ob.preseek_index.size; --i >= 0; )
		ob.preseek_len[i] = ob.chunks[i].len;
	ob.preseek_state = ob.state;
	ob.state = OB_STATE_SEEK;
	reader_reset_buffer();
	return AS_INPROGRESS;
}

static enum action_status ob_do_seek_finish(void)
{
	assert(pthread_equal(pthread_self(), ob.thread));
	assert(ob.state == OB_STATE_SEEK);
	ob.state = ob.preseek_state;
	if (dc.seek_where < 0) {
		int i;
		assert(dc.seek_where == DC_SEEK_MISMATCH ||
		       dc.seek_where == DC_SEEK_ERROR);

		/* restore the old ringbuf index if we failed to seek */
		memcpy(ob.index, &ob.preseek_index, sizeof(struct ringbuf));
		for (i = ob.preseek_index.size; --i >= 0; )
			ob.chunks[i].len = ob.preseek_len[i];
	} else {
		assert(dc.seek_where >= 0);
		ob.xfade_state = XFADE_DISABLED;
		ob.elapsed_time = dc.seek_where;
		ob.total_time = dc.total_time;
		reader_reset_buffer();
		dropBufferedAudio();
		ob_seq_player_set((unsigned int)ob.seq_decoder);
	}
	return ob_finalize_action();
}

static enum action_status ob_take_action(void)
{
	assert(pthread_equal(pthread_self(), ob.thread));
	if (mpd_likely(ob.action == OB_ACTION_NONE))
		return AS_COMPLETE;
	DEBUG(__FILE__": %s %d\n", __func__, __LINE__);
	cond_enter(&ob_action_cond);
	DEBUG(__FILE__": %s %d action: %d\n", __func__, __LINE__, ob.action);
	switch (ob.action) {
	case OB_ACTION_NONE: return ob_finalize_action();
	case OB_ACTION_PLAY: ob.state = OB_STATE_PLAY; break;
	case OB_ACTION_DROP: return ob_do_drop();
	case OB_ACTION_SEEK_START: return ob_do_seek_start();
	case OB_ACTION_SEEK_FINISH: return ob_do_seek_finish();
	case OB_ACTION_PAUSE_SET:
		if (ob.state == OB_STATE_PLAY)
			return ob_do_pause();
		ob.state = OB_STATE_PAUSE;
		break;
	case OB_ACTION_PAUSE_UNSET:
		if (ob.state == OB_STATE_PAUSE)
			ob.state = OB_STATE_PLAY;
		break;
	case OB_ACTION_PAUSE_FLIP:
		switch (ob.state) {
		case OB_STATE_PLAY: return ob_do_pause();
		case OB_STATE_PAUSE: ob.state = OB_STATE_PLAY; break;
		default: break;
		}
		break;
	case OB_ACTION_STOP: return ob_do_stop();
	case OB_ACTION_RESET: return ob_do_reset(0);
	case OB_ACTION_QUIT:
		dropBufferedAudio();
		closeAudioDevice();
		ob.state = OB_STATE_QUIT;
		return AS_INPROGRESS;
	}
	return ob_finalize_action();
}

/*
 * looks up the chunk given by index `i', returns NULL if `i' is beyond
 * the end of the buffer.  This allows us to treat our chunks array
 * like an infinite, rotating buffer.  The first available chunk
 * is always indexed as `0', the second one as `1', and so on...
 */
static struct ob_chunk *get_chunk(struct iovec vec[2], size_t i)
{
	if (vec[0].iov_len > i)
		return &ob.chunks[vec[0].iov_base + i - ob.index->buf];
	if (i && vec[1].iov_base) {
		assert(vec[0].iov_len > 0);
		i -= vec[0].iov_len;
		if (vec[1].iov_len > i)
			return &ob.chunks[vec[1].iov_base + i - ob.index->buf];
	}
	return NULL;
}

static void prevent_buffer_underrun(void)
{
	if (playAudio(silence.data, sizeof(silence.data)) < 0)
		stop_playback();
}

/* causes ob_do_drop() to be called (and waits for completion) */
void ob_drop_audio(enum ob_drop_type type)
{
	assert(!pthread_equal(pthread_self(), ob.thread));
	assert(!pthread_equal(pthread_self(), dc.thread));
	assert(dc.state == DC_STATE_STOP); /* not needed, just a good idea */
	cond_enter(&ob_seq_cond);
	switch (type) {
	case OB_DROP_DECODED: ob.seq_drop = ob.seq_decoder; break;
	case OB_DROP_PLAYING: ob.seq_drop = ob.seq_player; break;
	}
	cond_leave(&ob_seq_cond);
	/* DEBUG("dropping %u\n", ob.seq_drop); */
	ob_trigger_action(OB_ACTION_DROP);
	/* DEBUG("done dropping %u\n", ob.seq_drop); */
}

/* call this exactly once before decoding each song */
void ob_advance_sequence(void)
{
	assert(pthread_equal(pthread_self(), dc.thread));
	DEBUG(__FILE__": %s %d\n", __func__, __LINE__);
	cond_enter(&ob_seq_cond);
	++ob.seq_decoder;
	cond_leave(&ob_seq_cond);
	DEBUG(__FILE__": %s %d\n", __func__, __LINE__);
	DEBUG("ob.seq_decoder: %d\n", ob.seq_decoder);
}

/*
 * Returns true if output buffer is playing the song we're decoding
 */
int ob_synced(void)
{
	int ret;
	assert(!pthread_equal(pthread_self(), dc.thread));
	assert(!pthread_equal(pthread_self(), ob.thread));
	/* assert(pthread_equal(pthread_self(), main_thread)); */
	cond_enter(&ob_seq_cond);
	ret = (ob.seq_decoder == ob.seq_player);
	cond_leave(&ob_seq_cond);
	return ret;
}

static void new_song_chunk(struct ob_chunk *a)
{
	assert(pthread_equal(pthread_self(), ob.thread));
	ob.xfade_state = XFADE_DISABLED;
	ob.total_time = dc.total_time;
	/* DEBUG("ob.total_time: %f\n", ob.total_time); */
	ob_seq_player_set((unsigned int)a->seq);
	wakeup_main_task(); /* sync playlist */
}

#include "outputBuffer_audio.h"

static void play_next_chunk(void)
{
	struct iovec vec[2];
	struct ob_chunk *a;
	size_t nr;
	static float last_time;

	assert(pthread_equal(pthread_self(), ob.thread));

	nr = ringbuf_get_read_vector(ob.index, vec);
	if (mpd_unlikely(!nr)) {
		if (dc.state == DC_STATE_STOP && ! playlist_playing())
			stop_playback();
		else
			prevent_buffer_underrun();
		return;
	}

	if (ob.xfade_time <= 0 && nr < ob.bpp_cur) {
		prevent_buffer_underrun();
		return;
	} else if (nr < xfade_chunks_needed(vec)) {
		if (dc.state != DC_STATE_STOP && playlist_playing()) {
			prevent_buffer_underrun();
			return;
		}
		/* nearing end of last track, xfade to silence.. */
	}

	a = get_chunk(vec, 0);
	assert(a);
	if (! a->len)
		goto out;

	if (ob.xfade_state == XFADE_ENABLED) {
		struct ob_chunk *b;
		b = get_chunk(vec, ob.xfade_max);
		if (!b) { /* xfade to silence */
			b = &silence;
			b->len = a->len;
			b->seq = a->seq + 1;
		}
		xfade_mix(a, b);
	}

	last_time = ob.elapsed_time = a->time;
	ob.bit_rate = a->bit_rate;

	if (mpd_unlikely(ob.seq_player != a->seq)) {
		if (open_audio_devices(1) < 0)
			return;
		new_song_chunk(a);
	}
	/* pcm_volumeChange(a->data, a->len, &ob.audio_format, ob.sw_vol); */
	if (playAudio(a->data, a->len) < 0)
		stop_playback();
	a->len = 0; /* mark the chunk as empty for ob_send() */
out:
	ringbuf_read_advance(ob.index, 1);

	/* we've played our first chunk, stop prebuffering */
	if (mpd_unlikely(ob.bpp_cur))
		ob.bpp_cur = 0;

	/* unblock ob_send() if it was waiting on a full buffer */
	dc_try_unhalt();
}

static void * ob_task(mpd_unused void *arg)
{
	enum action_status as;

	assert(pthread_equal(pthread_self(), ob.thread));
	cond_enter(&ob_halt_cond);
	while (1) {
		as = ob_take_action();
		switch (ob.state) {
		case OB_STATE_PLAY:
			assert(as == AS_COMPLETE);
			if (open_audio_devices(0) >= 0)
				play_next_chunk();
			break;
		case OB_STATE_STOP:
		case OB_STATE_PAUSE:
		case OB_STATE_SEEK:
			assert(as != AS_DEFERRED);
			ob.bpp_cur = ob.bpp_max; /* enable prebuffer */
			if (as == AS_INPROGRESS)
				ob_finalize_action();
			cond_wait(&ob_halt_cond);
			break;
		case OB_STATE_QUIT: goto out;
		}
	}
out:
	cond_leave(&ob_halt_cond);
	assert(ob.state == OB_STATE_QUIT);
	assert(as == AS_INPROGRESS);
	ob_finalize_action();
	return NULL;
}

#include "outputBuffer_config_init.h"

void ob_seek_start(void)
{
	assert(pthread_equal(pthread_self(), dc.thread));
	assert(dc.seek_where >= 0);
	ob_trigger_action(OB_ACTION_SEEK_START);
}

void ob_seek_finish(void)
{
	assert(pthread_equal(pthread_self(), dc.thread));
	ob_trigger_action(OB_ACTION_SEEK_FINISH);
}

/*
 * if there are any partially written chunk, flush them out to
 * the output process _before_ decoding the next track
 */
void ob_flush(void)
{
	struct iovec vec[2];

	assert(pthread_equal(pthread_self(), dc.thread));
	/* DEBUG(__FILE__":%s %d\n", __func__, __LINE__); */

	if (ringbuf_get_write_vector(ob.index, vec)) {
		/* DEBUG(__FILE__":%s %d\n", __func__, __LINE__); */
		struct ob_chunk *c = get_chunk(vec, 0);
		assert(c);
		if (c->len) {
			assert(ob.seq_decoder == c->seq);
			switch (ob.state) {
			case OB_STATE_SEEK:
				assert(0);
			case OB_STATE_PLAY:
			case OB_STATE_PAUSE:
				ringbuf_write_advance(ob.index, 1);
				break;
			case OB_STATE_STOP:
			case OB_STATE_QUIT:
				c->len = 0;
			}
		}
	}
}

#include "outputBuffer_ob_send.h"
