#include "songvec.h"
#include "myfprintf.h"
#include "utils.h"

/* Only used for sorting/searchin a songvec, not general purpose compares */
static int songvec_cmp(const void *s1, const void *s2)
{
	const Song *a = ((const Song * const *)s1)[0];
	const Song *b = ((const Song * const *)s2)[0];
	return strcmp(a->url, b->url);
}

static size_t sv_size(struct songvec *sv)
{
	return sv->nr * sizeof(Song *);
}

void songvec_sort(struct songvec *sv)
{
	qsort(sv->base, sv->nr, sizeof(Song *), songvec_cmp);
}

Song *songvec_find(struct songvec *sv, const char *url)
{
	int i;

	for (i = sv->nr; --i >= 0; )
		if (!strcmp(sv->base[i]->url, url))
			return sv->base[i];
	return NULL;
}

int songvec_delete(struct songvec *sv, const Song *del)
{
	int i;

	for (i = sv->nr; --i >= 0; ) {
		if (sv->base[i] != del)
			continue;
		/* we _don't_ call freeSong() here */
		if (!--sv->nr) {
			free(sv->base);
			sv->base = NULL;
		} else {
			memmove(&sv->base[i], &sv->base[i + 1],
				(sv->nr - i + 1) * sizeof(Song *));
			sv->base = xrealloc(sv->base, sv_size(sv));
		}
		return i;
	}

	return -1; /* not found */
}

void songvec_add(struct songvec *sv, Song *add)
{
	++sv->nr;
	sv->base = xrealloc(sv->base, sv_size(sv));
	sv->base[sv->nr - 1] = add;
}

void songvec_destroy(struct songvec *sv)
{
	if (sv->base) {
		free(sv->base);
		sv->base = NULL;
	}
	sv->nr = 0;
}

int songvec_write(struct songvec *sv, int fd, int extra)
{
	int i;
	Song **sp = sv->base;

	if (extra) {
		if (fdprintf(fd, SONG_BEGIN "\n") < 0)
			return -1;

		for (i = sv->nr; --i >= 0; ) {
			Song *song = *sp++;
			if (fdprintf(fd, SONG_KEY "%s\n", song->url) < 0)
				return -1;
			if (song_print_info(song, fd) < 0)
				return -1;
			if (fdprintf(fd,
			            SONG_MTIME "%li\n", (long)song->mtime) < 0)
				return -1;
		}

		if (fdprintf(fd, SONG_END "\n") < 0)
			return -1;
	} else {
		for (i = sv->nr; --i >= 0;)
			if (song_print_info(*sp++, fd) < 0)
				return -1;
	}

	return 0;
}
