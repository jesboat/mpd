#include "dirvec.h"
#include "os_compat.h"
#include "utils.h"

static size_t dv_size(struct dirvec *dv)
{
	return dv->nr * sizeof(Directory *);
}

/* Only used for sorting/searching a dirvec, not general purpose compares */
static int dirvec_cmp(const void *d1, const void *d2)
{
	const Directory *a = ((const Directory * const *)d1)[0];
	const Directory *b = ((const Directory * const *)d2)[0];
	return strcmp(a->path, b->path);
}

void dirvec_sort(struct dirvec *dv)
{
	qsort(dv->base, dv->nr, sizeof(Directory *), dirvec_cmp);
}

Directory *dirvec_find(struct dirvec *dv, const char *path)
{
	int i;

	for (i = dv->nr; --i >= 0; )
		if (!strcmp(dv->base[i]->path, path))
			return dv->base[i];
	return NULL;
}

int dirvec_delete(struct dirvec *dv, Directory *del)
{
	int i;

	for (i = dv->nr; --i >= 0; ) {
		if (dv->base[i] != del)
			continue;
		/* we _don't_ call freeDirectory() here */
		if (!--dv->nr) {
			free(dv->base);
			dv->base = NULL;
		} else {
			memmove(&dv->base[i], &dv->base[i + 1],
				(dv->nr - i + 1) * sizeof(Directory *));
			dv->base = xrealloc(dv->base, dv_size(dv));
		}
		return i;
	}

	return -1; /* not found */
}

void dirvec_add(struct dirvec *dv, Directory *add)
{
	++dv->nr;
	dv->base = xrealloc(dv->base, dv_size(dv));
	dv->base[dv->nr - 1] = add;
}

void dirvec_destroy(struct dirvec *dv)
{
	if (dv->base) {
		free(dv->base);
		dv->base = NULL;
	}
	dv->nr = 0;
}
