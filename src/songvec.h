#ifndef SONGVEC_H
#define SONGVEC_H

#include "song.h"
#include "os_compat.h"

struct songvec {
	Song **base;
	size_t nr;
};

void songvec_sort(struct songvec *sv);

Song *songvec_find(struct songvec *sv, const char *url);

int songvec_delete(struct songvec *sv, Song *del);

void songvec_add(struct songvec *sv, Song *add);

void songvec_free(struct songvec *sv);

int songvec_write(struct songvec *sv, int fd, int extra);

void songvec_prune(struct songvec *sv);

#endif /* SONGVEC_H */
