/*
 * Copyright (C) 2003-2012 The Music Player Daemon Project
 * http://www.musicpd.org
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
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "config.h"
#include "AdPlugDecoderPlugin.h"
#include "tag_handler.h"
#include "decoder_api.h"

extern "C" {
#include "audio_check.h"
}

#include <adplug/adplug.h>
#include <adplug/emuopl.h>

#include <glib.h>

#include <assert.h>

#undef G_LOG_DOMAIN
#define G_LOG_DOMAIN "adplug"

static unsigned sample_rate;

static bool
adplug_init(const struct config_param *param)
{
	GError *error = NULL;

	sample_rate = config_get_block_unsigned(param, "sample_rate", 48000);
	if (!audio_check_sample_rate(sample_rate, &error)) {
		g_warning("%s\n", error->message);
		g_error_free(error);
		return false;
	}

	return true;
}

static void
adplug_file_decode(struct decoder *decoder, const char *path_fs)
{
	CEmuopl opl(sample_rate, true, true);
	opl.init();

	CPlayer *player = CAdPlug::factory(path_fs, &opl);
	if (player == nullptr)
		return;

	struct audio_format audio_format;
	audio_format_init(&audio_format, sample_rate, SAMPLE_FORMAT_S16, 2);
	assert(audio_format_valid(&audio_format));

	decoder_initialized(decoder, &audio_format, false,
		player->songlength() / 1000.);

	int16_t buffer[2048];
	const unsigned frames_per_buffer = G_N_ELEMENTS(buffer) / 2;
	enum decoder_command cmd;

	do {
		if (!player->update())
			break;

		opl.update(buffer, frames_per_buffer);
		cmd = decoder_data(decoder, NULL,
				   buffer, sizeof(buffer),
				   0);
	} while (cmd == DECODE_COMMAND_NONE);

	delete player;
}

static void
adplug_scan_tag(enum tag_type type, const std::string &value,
		const struct tag_handler *handler, void *handler_ctx)
{
	if (!value.empty())
		tag_handler_invoke_tag(handler, handler_ctx,
				       type, value.c_str());
}

static bool
adplug_scan_file(const char *path_fs,
		 const struct tag_handler *handler, void *handler_ctx)
{
	CEmuopl opl(sample_rate, true, true);
	opl.init();

	CPlayer *player = CAdPlug::factory(path_fs, &opl);
	if (player == nullptr)
		return false;

	tag_handler_invoke_duration(handler, handler_ctx,
				    player->songlength() / 1000);

	if (handler->tag != nullptr) {
		adplug_scan_tag(TAG_TITLE, player->gettitle(),
				handler, handler_ctx);
		adplug_scan_tag(TAG_ARTIST, player->getauthor(),
				handler, handler_ctx);
		adplug_scan_tag(TAG_COMMENT, player->getdesc(),
				handler, handler_ctx);
	}

	delete player;
	return true;
}

static const char *const adplug_suffixes[] = {
	"amd",
	"d00",
	"hsc",
	"laa",
	"rad",
	"raw",
	"sa2",
	nullptr
};

const struct decoder_plugin adplug_decoder_plugin = {
	"adplug",
	adplug_init,
	nullptr,
	nullptr,
	adplug_file_decode,
	adplug_scan_file,
	nullptr,
	nullptr,
	adplug_suffixes,
	nullptr,
};
