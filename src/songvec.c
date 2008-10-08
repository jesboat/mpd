#include "songvec.h"
#include "myfprintf.h"
#include "utils.h"

static pthread_mutex_t nr_lock = PTHREAD_MUTEX_INITIALIZER;

/* Only used for sorting/searchin a songvec, not general purpose compares */
static int songvec_cmp(const void *s1, const void *s2)
{
	const struct mpd_song *a = ((const struct mpd_song * const *)s1)[0];
	const struct mpd_song *b = ((const struct mpd_song * const *)s2)[0];
	return strcmp(a->url, b->url);
}

static size_t sv_size(struct songvec *sv)
{
	return sv->nr * sizeof(struct mpd_song *);
}

void songvec_sort(struct songvec *sv)
{
	pthread_mutex_lock(&nr_lock);
	qsort(sv->base, sv->nr, sizeof(struct mpd_song *), songvec_cmp);
	pthread_mutex_unlock(&nr_lock);
}

struct mpd_song *songvec_find(struct songvec *sv, const char *url)
{
	int i;
	struct mpd_song *ret = NULL;

	pthread_mutex_lock(&nr_lock);
	for (i = sv->nr; --i >= 0; ) {
		if (strcmp(sv->base[i]->url, url))
			continue;
		ret = sv->base[i];
		break;
	}
	pthread_mutex_unlock(&nr_lock);
	return ret;
}

int songvec_delete(struct songvec *sv, const struct mpd_song *del)
{
	int i;

	pthread_mutex_lock(&nr_lock);
	for (i = sv->nr; --i >= 0; ) {
		if (sv->base[i] != del)
			continue;
		/* we _don't_ call freeSong() here */
		if (!--sv->nr) {
			free(sv->base);
			sv->base = NULL;
		} else {
			memmove(&sv->base[i], &sv->base[i + 1],
				(sv->nr - i + 1) * sizeof(struct mpd_song *));
			sv->base = xrealloc(sv->base, sv_size(sv));
		}
		break;
	}
	pthread_mutex_unlock(&nr_lock);

	return i;
}

void songvec_add(struct songvec *sv, struct mpd_song *add)
{
	pthread_mutex_lock(&nr_lock);
	++sv->nr;
	sv->base = xrealloc(sv->base, sv_size(sv));
	sv->base[sv->nr - 1] = add;
	pthread_mutex_unlock(&nr_lock);
}

void songvec_destroy(struct songvec *sv)
{
	pthread_mutex_lock(&nr_lock);
	if (sv->base) {
		free(sv->base);
		sv->base = NULL;
	}
	sv->nr = 0;
	pthread_mutex_unlock(&nr_lock);
}

int songvec_for_each(const struct songvec *sv,
                     int (*fn)(struct mpd_song *, void *), void *arg)
{
	size_t i;

	pthread_mutex_lock(&nr_lock);
	for (i = 0; i < sv->nr; ++i) {
		struct mpd_song *song = sv->base[i];

		assert(song);
		assert(*song->url);

		pthread_mutex_unlock(&nr_lock); /* fn() may block */
		if (fn(song, arg) < 0)
			return -1;
		pthread_mutex_lock(&nr_lock); /* sv->nr may change in fn() */
	}
	pthread_mutex_unlock(&nr_lock);

	return 0;
}
