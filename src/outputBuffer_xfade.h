#ifndef OUTPUT_BUFFER_XFADE_H
#define OUTPUT_BUFFER_XFADE_H

#include "os_compat.h"
#include "audio.h"
#include "pcm_utils.h"

static struct ob_chunk *get_chunk(struct iovec vec[2], size_t i);
static size_t calculate_xfade_chunks(struct iovec vec[2])
{
	size_t chunks;
	struct ob_chunk *c;
	size_t nr;
	AudioFormat *af = &ob.audio_format;

	assert(pthread_equal(ob.thread, pthread_self()));

	if (!ob.total_time ||
	    (ob.elapsed_time + ob.xfade_time) < ob.total_time ||
	    !isCurrentAudioFormat(af))
		return ob.nr_bpp; /* too early, don't enable xfade yet */

	assert(af->bits > 0);
	assert(af->channels > 0);
	assert(af->sampleRate > 0);

	chunks = af->sampleRate * af->bits * af->channels / 8.0 / CHUNK_SIZE;
	chunks = chunks * (ob.xfade_time + 0.5);

	assert(ob.index->size >= ob.nr_bpp);
	if (chunks > (ob.index->size - ob.nr_bpp))
		chunks = ob.index->size - ob.nr_bpp;
	DEBUG("calculated xfade chunks: %d\n", chunks);
	nr = vec[0].iov_len + vec[1].iov_len;

	if (chunks > nr)
		return chunks; /* not enough work with */

	c = get_chunk(vec, chunks);
	assert(c);
	if (c->seq == ob.seq_player) {
		do {
			if (!(c = get_chunk(vec, ++chunks)))
				return chunks; /* not enough to work with */
		} while (c->seq == ob.seq_player);
	} else {
		do {
			c = get_chunk(vec, --chunks);
			assert(c);
		} while (c->seq == ob.seq_decoder);
		assert((c = get_chunk(vec, chunks)));
		assert(c->seq != ob.seq_decoder);
		++chunks;
		assert((c = get_chunk(vec, chunks)));
		assert(c->seq == ob.seq_decoder);
	}
	DEBUG("adjusted xfade chunks: %d\n", chunks);

	ob.xfade_cur = chunks;
	ob.xfade_max = chunks;
	assert(ob.xfade_state == XFADE_DISABLED);
	ob.xfade_state = XFADE_ENABLED;
	return chunks;
}

static size_t xfade_chunks_needed(struct iovec vec[2])
{
	assert(pthread_equal(ob.thread, pthread_self()));

	if (ob.xfade_state == XFADE_DISABLED)
		return calculate_xfade_chunks(vec);
	assert(ob.xfade_state == XFADE_ENABLED);
	return ob.xfade_max;
}

static void xfade_mix(struct ob_chunk *a, struct ob_chunk *b)
{
	assert(pthread_equal(ob.thread, pthread_self()));
	assert(ob.xfade_state == XFADE_ENABLED);
	assert(ob.xfade_cur <= ob.xfade_max);
	assert(b);
	assert(a != b);
	assert(a->len <= CHUNK_SIZE);
	assert(b->len <= CHUNK_SIZE);
	if (b->seq == a->seq) {
		/* deal with small rounding errors */
		DEBUG("seq_match: %d == %d\n", a->seq, b->seq);
		return;
	}

	/* as xfade_cur increases, b is scaled more and a is scaled less */
	pcm_mix(a->data, b->data, a->len, b->len,
	        &ob.audio_format, ((float)ob.xfade_cur) / ob.xfade_max);

	/* next time we fade more until we have nothing to fade */
	if (ob.xfade_cur)
		--ob.xfade_cur;
	if (b->len > a->len) {
		DEBUG("reassign len: %d => %d\n", a->len, b->len);
		a->len = b->len;
	}
	b->len = 0; /* invalidate the chunk we already mixed in */
}

#endif /* OUTPUT_BUFFER_XFADE_H */
