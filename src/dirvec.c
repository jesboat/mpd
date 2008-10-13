#include "dirvec.h"
#include "directory.h"
#include "os_compat.h"
#include "utils.h"

static pthread_mutex_t nr_lock = PTHREAD_MUTEX_INITIALIZER;

static size_t dv_size(const struct dirvec *dv)
{
	return dv->nr * sizeof(struct directory *);
}

/* Only used for sorting/searching a dirvec, not general purpose compares */
static int dirvec_cmp(const void *d1, const void *d2)
{
	const struct directory *a = ((const struct directory * const *)d1)[0];
	const struct directory *b = ((const struct directory * const *)d2)[0];
	return strcmp(a->path, b->path);
}

struct directory *dirvec_find(const struct dirvec *dv, const char *path)
{
	int i;
	struct directory *ret = NULL;

	pthread_mutex_lock(&nr_lock);
	for (i = dv->nr; --i >= 0; ) {
		if (strcmp(dv->base[i]->path, path))
			continue;
		ret = dv->base[i];
		break;
	}
	pthread_mutex_unlock(&nr_lock);
	return ret;
}

int dirvec_delete(struct dirvec *dv, struct directory *del)
{
	size_t i;

	pthread_mutex_lock(&nr_lock);
	for (i = 0; i < dv->nr; ++i) {
		if (dv->base[i] != del)
			continue;
		/* we _don't_ call directory_free() here */
		if (!--dv->nr) {
			free(dv->base);
			dv->base = NULL;
		} else {
			if (i < dv->nr)
				memmove(&dv->base[i], &dv->base[i + 1],
					(dv->nr - i) *
					sizeof(struct directory *));
			dv->base = xrealloc(dv->base, dv_size(dv));
		}
		pthread_mutex_unlock(&nr_lock);
		return i;
	}
	pthread_mutex_unlock(&nr_lock);

	return -1; /* not found */
}

void dirvec_add(struct dirvec *dv, struct directory *add)
{
	size_t old_nr;

	pthread_mutex_lock(&nr_lock);
	old_nr = dv->nr++;
	dv->base = xrealloc(dv->base, dv_size(dv));
	dv->base[old_nr] = add;
	if (old_nr && dirvec_cmp(&dv->base[old_nr - 1], &add) >= 0)
		qsort(dv->base, dv->nr, sizeof(struct directory *), dirvec_cmp);
	pthread_mutex_unlock(&nr_lock);
}

void dirvec_destroy(struct dirvec *dv)
{
	pthread_mutex_lock(&nr_lock);
	dv->nr = 0;
	pthread_mutex_unlock(&nr_lock);
	if (dv->base) {
		free(dv->base);
		dv->base = NULL;
	}
}

int dirvec_for_each(const struct dirvec *dv,
                    int (*fn)(struct directory *, void *), void *arg)
{
	size_t i;
	size_t prev_nr;

	pthread_mutex_lock(&nr_lock);
	for (i = 0; i < dv->nr; ) {
		struct directory *dir = dv->base[i];

		assert(dir);
		prev_nr = dv->nr;
		pthread_mutex_unlock(&nr_lock);
		if (fn(dir, arg) < 0)
			return -1;
		pthread_mutex_lock(&nr_lock); /* dv->nr may change in fn() */
		if (prev_nr == dv->nr)
			++i;
	}
	pthread_mutex_unlock(&nr_lock);

	return 0;
}
