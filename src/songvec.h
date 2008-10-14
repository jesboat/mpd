#ifndef SONGVEC_H
#define SONGVEC_H

#include "song.h"
#include "os_compat.h"

struct songvec {
	struct mpd_song **base;
	size_t nr;
};

struct mpd_song *songvec_find(const struct songvec *sv, const char *url);

int songvec_delete(struct songvec *sv, const struct mpd_song *del);

void songvec_add(struct songvec *sv, struct mpd_song *add);

int songvec_for_each(const struct songvec *sv,
                     int (*fn)(struct mpd_song *, void *), void *arg);

#endif /* SONGVEC_H */
