#include "player_error.h"
#include "os_compat.h"
#include "log.h"
#include "path.h"

enum player_error player_errno;
Song *player_errsong;

void player_clearerror(void)
{
	player_errno = PLAYER_ERROR_NONE;
	player_errsong = NULL;
}

void player_seterror(enum player_error err, Song *song)
{
	if (player_errno)
		ERROR("Clobbering existing error: %s\n", player_strerror());
	player_errno = err;
	player_errsong = song;
}

const char *player_strerror(void)
{
	/* static OK here, only one user in main task */
	static char error[MPD_PATH_MAX + 64]; /* still too much */
	char path_max_tmp[MPD_PATH_MAX];
	*error = '\0'; /* likely */

	switch (player_errno) {
	case PLAYER_ERROR_NONE: break;
	case PLAYER_ERROR_FILE:
		snprintf(error, sizeof(error), "problems decoding \"%s\"",
			 get_song_url(path_max_tmp, player_errsong));
		break;
	case PLAYER_ERROR_AUDIO:
		strcpy(error, "problems opening audio device");
		break;
	case PLAYER_ERROR_SYSTEM:
		strcpy(error, "system error occured");
		break;
	case PLAYER_ERROR_UNKTYPE:
		snprintf(error, sizeof(error), "file type of \"%s\" is unknown",
			 get_song_url(path_max_tmp, player_errsong));
	case PLAYER_ERROR_FILENOTFOUND:
		snprintf(error, sizeof(error),
			 "file \"%s\" does not exist or is inaccessible",
			 get_song_url(path_max_tmp, player_errsong));
		break;
	}
	return *error ? error : NULL;
}

