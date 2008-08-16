/* This is where audio devices are managed inside the output buffer thread */

static int audio_opened;

/*
 * reopen is set when we get a new song and there's a difference
 * in audio format
 */
static int open_audio_devices(int reopen)
{
	assert(pthread_equal(pthread_self(), ob.thread));

	if (!reopen && audio_opened)
		return 0;
	if (openAudioDevice(&ob.audio_format) >= 0) {
		audio_opened = 1;
		return 0;
	}
	audio_opened = 0;
	stop_playback();
	player_seterror(PLAYER_ERROR_AUDIO, NULL);
	ERROR("problems opening audio device\n");
	return -1;
}

static void close_audio_devices(void)
{
	assert(pthread_equal(pthread_self(), ob.thread));
	DEBUG(__FILE__":%s %d\n", __func__, __LINE__);
	dropBufferedAudio();
	closeAudioDevice();
	audio_opened = 0;
	/* DEBUG(__FILE__":%s %d\n", __func__, __LINE__); */
}

