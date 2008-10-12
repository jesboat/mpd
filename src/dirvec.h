#ifndef DIRVEC_H
#define DIRVEC_H

#include "os_compat.h"

struct dirvec {
	struct directory **base;
	size_t nr;
};

struct directory *dirvec_find(const struct dirvec *dv, const char *path);

int dirvec_delete(struct dirvec *dv, struct directory *del);

void dirvec_add(struct dirvec *dv, struct directory *add);

static inline void dirvec_clear(struct dirvec *dv)
{
	dv->nr = 0;
}

void dirvec_destroy(struct dirvec *dv);

int dirvec_for_each(const struct dirvec *dv,
                    int (*fn)(struct directory *, void *), void *arg);

#endif /* DIRVEC_H */
