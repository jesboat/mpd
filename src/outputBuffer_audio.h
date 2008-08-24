/*
 * reopen is set when we get a new song and there's a difference
 * in audio format
 */
static int open_audio_devices(int reopen)
{
	assert(pthread_equal(pthread_self(), ob.thread));

	if (!reopen && isAudioDeviceOpen())
		return 0;

	if (openAudioDevice(&ob.audio_format) >= 0)
		return 0;
	stop_playback();
	player_seterror(PLAYER_ERROR_AUDIO, NULL);
	ERROR("problems opening audio device\n");
	return -1;
}

