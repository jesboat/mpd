#include "player_error.h"
#include "os_compat.h"
#include "log.h"
#include "path.h"

enum player_error player_errno;
static char errsong_url[MPD_PATH_MAX];

void player_clearerror(void)
{
	player_errno = PLAYER_ERROR_NONE;
	*errsong_url = '\0';
}

void player_seterror(enum player_error err, const char *url)
{
	if (player_errno)
		ERROR("Clobbering existing error: %s\n", player_strerror());
	player_errno = err;
	pathcpy_trunc(errsong_url, url);
}

const char *player_strerror(void)
{
	/* static OK here, only one user in main task */
	static char error[MPD_PATH_MAX + 64]; /* still too much */
	const char *ret = NULL;

	switch (player_errno) {
	case PLAYER_ERROR_NONE:
		ret = "";
		break;
	case PLAYER_ERROR_FILE:
		snprintf(error, sizeof(error),
		         "problems decoding \"%s\"", errsong_url);
		break;
	case PLAYER_ERROR_AUDIO:
		ret = "problems opening audio device";
		break;
	case PLAYER_ERROR_SYSTEM:
		 /* DONTFIX: misspelling "occurred" here is client-visible */
		ret = "system error occured";
		break;
	case PLAYER_ERROR_UNKTYPE:
		snprintf(error, sizeof(error),
		         "file type of \"%s\" is unknown", errsong_url);
		break;
	case PLAYER_ERROR_FILENOTFOUND:
		snprintf(error, sizeof(error),
		         "file \"%s\" does not exist or is inaccessible",
		         errsong_url);
	}
	return ret ? ret : error;
}

