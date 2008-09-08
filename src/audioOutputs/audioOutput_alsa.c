/* the Music Player Daemon (MPD)
 * Copyright (C) 2003-2007 by Warren Dukes (warren.dukes@gmail.com)
 * This project's homepage is: http://www.musicpd.org
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "../audioOutput.h"

#ifdef HAVE_ALSA

#define ALSA_PCM_NEW_HW_PARAMS_API
#define ALSA_PCM_NEW_SW_PARAMS_API

static const char default_device[] = "default";

#define MPD_ALSA_BUFFER_TIME_US 500000
/* the default period time of xmms is 50 ms, so let's use that as well.
 * a user can tweak this parameter via the "period_time" config parameter.
 */
#define MPD_ALSA_PERIOD_TIME_US 50000
#define MPD_ALSA_RETRY_NR 5

#include "../conf.h"
#include "../log.h"
#include "../os_compat.h"

#include <alsa/asoundlib.h>

/* #define MPD_SND_PCM_NONBLOCK SND_PCM_NONBLOCK */
#define MPD_SND_PCM_NONBLOCK 0

/*
 * This macro will evaluate both statements, but only returns the result
 * of the second statement to the reader.  Thus it'll stringify the
 * command name and assign it to the scoped cmd variable.
 * Note that ALSA is strictly for Linux , and anybody compiling
 * on Linux will have gcc or a gcc-compatible compiler anyways.
 */
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 199901L
#  define E(command , arg1 , ...) \
	(err_cmd = #command, command( arg1 , __VA_ARGS__ ))
#else /* ! C99, this works for gcc 2.95 at least: */
#  define E(command , arg1 , args...) \
	(err_cmd = #command, command( arg1 , ##args ))
#endif /* ! C99 */

typedef snd_pcm_sframes_t alsa_writei_t(snd_pcm_t * pcm, const void *buffer,
					snd_pcm_uframes_t size);

typedef struct _AlsaData {
	const char *device;
	snd_pcm_t *pcmHandle;
	alsa_writei_t *writei;
	unsigned int buffer_time;
	unsigned int period_time;
	int sampleSize;
	int useMmap;
} AlsaData;

static AlsaData *newAlsaData(void)
{
	AlsaData *ret = xmalloc(sizeof(AlsaData));

	ret->device = default_device;
	ret->pcmHandle = NULL;
	ret->writei = snd_pcm_writei;
	ret->useMmap = 0;
	ret->buffer_time = MPD_ALSA_BUFFER_TIME_US;
	ret->period_time = MPD_ALSA_PERIOD_TIME_US;

	return ret;
}

static void freeAlsaData(AlsaData * ad)
{
	if (ad->device && ad->device != default_device)
		free(deconst_ptr(ad->device));
	free(ad);
}

static int alsa_initDriver(AudioOutput * audioOutput, ConfigParam * param)
{
	/* no need for pthread_once thread-safety when reading config */
	static int free_global_registered;
	AlsaData *ad = newAlsaData();

	if (!free_global_registered) {
		atexit((void(*)(void))snd_config_update_free_global);
		free_global_registered = 1;
	}

	if (param) {
		BlockParam *bp;

		if ((bp = getBlockParam(param, "device")))
			ad->device = xstrdup(bp->value);
		ad->useMmap = getBoolBlockParam(param, "use_mmap", 1);
		if (ad->useMmap == CONF_BOOL_UNSET)
			ad->useMmap = 0;
		if ((bp = getBlockParam(param, "buffer_time")))
			ad->buffer_time = atoi(bp->value);
		if ((bp = getBlockParam(param, "period_time")))
			ad->period_time = atoi(bp->value);
	}
	audioOutput->data = ad;

	return 0;
}

static void alsa_finishDriver(AudioOutput * audioOutput)
{
	AlsaData *ad = audioOutput->data;

	freeAlsaData(ad);
}

static int alsa_testDefault(void)
{
	snd_pcm_t *handle;

	int ret = snd_pcm_open(&handle, default_device,
	                       SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK);
	if (ret) {
		WARNING("Error opening default ALSA device: %s\n",
			snd_strerror(-ret));
		return -1;
	} else
		snd_pcm_close(handle);

	return 0;
}

static snd_pcm_format_t get_bitformat(const AudioFormat * af)
{
	switch (af->bits) {
	case 8: return SND_PCM_FORMAT_S8;
	case 16: return SND_PCM_FORMAT_S16;
	case 24: return SND_PCM_FORMAT_S24;
	case 32: return SND_PCM_FORMAT_S32;
	}
	return SND_PCM_FORMAT_UNKNOWN;
}

static int alsa_openDevice(AudioOutput * audioOutput)
{
	AlsaData *ad = audioOutput->data;
	AudioFormat *audioFormat = &audioOutput->outAudioFormat;
	snd_pcm_format_t bitformat;
	snd_pcm_hw_params_t *hwparams;
	snd_pcm_sw_params_t *swparams;
	unsigned int sampleRate = audioFormat->sampleRate;
	unsigned int channels = audioFormat->channels;
	snd_pcm_uframes_t buffer_size;
	snd_pcm_uframes_t period_size;
	int err;
	const char *err_cmd = NULL;
	int retry = MPD_ALSA_RETRY_NR;
	unsigned int period_time, period_time_ro;
	unsigned int buffer_time;

	if ((bitformat = get_bitformat(audioFormat)) == SND_PCM_FORMAT_UNKNOWN)
		ERROR("ALSA device \"%s\" doesn't support %i bit audio\n",
		      ad->device, audioFormat->bits);

	err = E(snd_pcm_open, &ad->pcmHandle, ad->device,
	        SND_PCM_STREAM_PLAYBACK, MPD_SND_PCM_NONBLOCK);
	if (err < 0) {
		ad->pcmHandle = NULL;
		goto error;
	}

#if MPD_SND_PCM_NONBLOCK == SND_PCM_NONBLOCK
	if ((err = E(snd_pcm_nonblock, ad->pcmHandle, 0)) < 0)
		goto error;
#endif /* MPD_SND_PCM_NONBLOCK == SND_PCM_NONBLOCK */

	period_time_ro = period_time = ad->period_time;
configure_hw:
	/* configure HW params */
	snd_pcm_hw_params_alloca(&hwparams);
	if ((err = E(snd_pcm_hw_params_any, ad->pcmHandle, hwparams)) < 0)
		goto error;

	if (ad->useMmap) {
		if (!(err = snd_pcm_hw_params_set_access(ad->pcmHandle,
		                hwparams, SND_PCM_ACCESS_MMAP_INTERLEAVED))) {
			ad->writei = snd_pcm_mmap_writei;
		} else {
			ERROR("ALSA cannot enable mmap on device \"%s\": %s. "
			      "Falling back to direct write mode\n",
			      ad->device, snd_strerror(-err));
			ad->useMmap = 0;
		}
	} else if ((err = E(snd_pcm_hw_params_set_access, ad->pcmHandle,
		            hwparams, SND_PCM_ACCESS_RW_INTERLEAVED)) < 0)
		goto error;

	err = snd_pcm_hw_params_set_format(ad->pcmHandle, hwparams, bitformat);
	if (err < 0) {
		ERROR("ALSA device \"%s\" does not support %i bit audio:%s\n",
		      ad->device, audioFormat->bits, snd_strerror(-err));
		goto fail;
	}
	err = snd_pcm_hw_params_set_channels_near(ad->pcmHandle, hwparams,
						  &channels);
	if (err < 0) {
		ERROR("ALSA device \"%s\" does not support %i channels: "
		      "%s\n", ad->device, (int)audioFormat->channels,
		      snd_strerror(-err));
		goto fail;
	}
	audioFormat->channels = (mpd_sint8)channels;

	err = snd_pcm_hw_params_set_rate_near(ad->pcmHandle, hwparams,
					      &sampleRate, NULL);
	if (err < 0 || sampleRate == 0) {
		ERROR("ALSA device \"%s\" does not support %i Hz audio\n",
		      ad->device, (int)audioFormat->sampleRate);
		goto fail;
	}
	audioFormat->sampleRate = sampleRate;

	buffer_time = ad->buffer_time;
	if ((err = E(snd_pcm_hw_params_set_buffer_time_near, ad->pcmHandle,
	             hwparams, &buffer_time, NULL)) < 0)
		goto error;

	period_time = period_time_ro;

	if ((err = E(snd_pcm_hw_params_set_period_time_near,
	             ad->pcmHandle, hwparams, &period_time, NULL)) < 0)
		goto error;

	err = E(snd_pcm_hw_params, ad->pcmHandle, hwparams);
	if (err == -EPIPE && --retry > 0) {
		period_time_ro = period_time_ro >> 1;
		goto configure_hw;
	} else if (err < 0)
		goto error;

	DEBUG("ALSA(%s) period_time: %u, buffer_time: %u\n",
	      ad->device, period_time, buffer_time);
	if ((err = E(snd_pcm_hw_params_get_buffer_size, hwparams,
	             &buffer_size)) < 0)
		goto error;

	if ((err = E(snd_pcm_hw_params_get_period_size, hwparams,
	             &period_size, NULL)) < 0)
		goto error;
	DEBUG("ALSA(%s) period_size: %lu buffer_size: %lu\n",
	      ad->device, period_size, buffer_size);

	/* configure SW params */
	snd_pcm_sw_params_alloca(&swparams);

	if ((err = E(snd_pcm_sw_params_current, ad->pcmHandle, swparams)) < 0)
		goto error;
	if ((err = E(snd_pcm_sw_params_set_start_threshold, ad->pcmHandle,
	              swparams, buffer_size - period_size)) < 0)
		goto error;
	if ((err = E(snd_pcm_sw_params_set_avail_min, ad->pcmHandle,
	             swparams, period_size)) < 0)
		goto error;
	if ((err = E(snd_pcm_sw_params, ad->pcmHandle, swparams)) < 0)
		goto error;

	ad->sampleSize = (audioFormat->bits / 8) * audioFormat->channels;

	audioOutput->open = 1;

	DEBUG("ALSA device \"%s\" will be playing %i bit, %i channel audio at "
	      "%i Hz\n", ad->device, (int)audioFormat->bits,
	      channels, sampleRate);

	return 0;

error:
	ERROR("Error opening ALSA device \"%s\" (%s): %s\n",
	      ad->device, (err_cmd ? err_cmd : ""), snd_strerror(-err));
fail:
	if (ad->pcmHandle)
		snd_pcm_close(ad->pcmHandle);
	ad->pcmHandle = NULL;
	audioOutput->open = 0;
	return -1;
}

static int alsa_errorRecovery(AlsaData * ad, int err)
{
	snd_pcm_state_t state = snd_pcm_state(ad->pcmHandle);
	const char *err_cmd = NULL;

	if (err == -EPIPE)
		DEBUG("Underrun on ALSA device \"%s\"\n", ad->device);
	else if (err == -ESTRPIPE)
		DEBUG("ALSA device \"%s\" was suspended\n", ad->device);

	switch (state) {
	case SND_PCM_STATE_PAUSED:
		err = E(snd_pcm_pause, ad->pcmHandle, /* disable */ 0);
		break;
	case SND_PCM_STATE_SUSPENDED:
		if ((err = E(snd_pcm_resume, ad->pcmHandle)) == -EAGAIN)
			return 0;
		/* fall-through to snd_pcm_prepare: */
	case SND_PCM_STATE_SETUP:
	case SND_PCM_STATE_XRUN:
		err = E(snd_pcm_prepare, ad->pcmHandle);
		break;
	case SND_PCM_STATE_DISCONNECTED:
		/* so alsa_closeDevice won't try to drain: */
		snd_pcm_close(ad->pcmHandle);
		ad->pcmHandle = NULL;
		break;
	/* this is no error, so just keep running */
	case SND_PCM_STATE_RUNNING:
		if (mpd_unlikely(err)) {
			DEBUG("ALSA(%s) ignoring possible error: %s\n",
			      ad->device, snd_strerror(-err));
			err = 0;
		}
		break;
	default:
		DEBUG("ALSA device \"%s\" in unknown state: %s\n",
		      ad->device, snd_pcm_state_name(state));
		break;
	}
	if (err && err_cmd)
		ERROR("ALSA error on device \"%s\" (%s): %s\n",
		      ad->device, err_cmd, snd_strerror(-err));
	return err;
}

static void alsa_dropBufferedAudio(AudioOutput * audioOutput)
{
	AlsaData *ad = audioOutput->data;

	alsa_errorRecovery(ad, snd_pcm_drop(ad->pcmHandle));
}

static void alsa_closeDevice(AudioOutput * audioOutput)
{
	AlsaData *ad = audioOutput->data;

	if (ad->pcmHandle) {
		if (snd_pcm_state(ad->pcmHandle) == SND_PCM_STATE_RUNNING) {
			snd_pcm_drain(ad->pcmHandle);
		}
		snd_pcm_close(ad->pcmHandle);
		ad->pcmHandle = NULL;
	}

	audioOutput->open = 0;
}

static int alsa_playAudio(AudioOutput * audioOutput,
			  const char *playChunk, size_t size)
{
	AlsaData *ad = audioOutput->data;
	int ret;

	size /= ad->sampleSize;

	while (size > 0) {
		ret = ad->writei(ad->pcmHandle, playChunk, size);

		if (ret == -EAGAIN || ret == -EINTR)
			continue;

		if (ret < 0) {
			if (alsa_errorRecovery(ad, ret) < 0) {
				ERROR("closing ALSA device \"%s\" due to write "
				      "error: %s\n", ad->device,
				      snd_strerror(-errno));
				alsa_closeDevice(audioOutput);
				return -1;
			}
			continue;
		}

		playChunk += ret * ad->sampleSize;
		size -= ret;
	}

	return 0;
}

AudioOutputPlugin alsaPlugin = {
	"alsa",
	alsa_testDefault,
	alsa_initDriver,
	alsa_finishDriver,
	alsa_openDevice,
	alsa_playAudio,
	alsa_dropBufferedAudio,
	alsa_closeDevice,
	NULL,	/* sendMetadataFunc */
};

#else /* HAVE ALSA */

DISABLED_AUDIO_OUTPUT_PLUGIN(alsaPlugin)
#endif /* HAVE_ALSA */
