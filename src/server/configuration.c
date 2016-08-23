
/*
 * dc_decl.h - Dotconf functions and types for Speech Dispatcher
 *
 * Copyright (C) 2001, 2002, 2003 Brailcom, o.p.s.
 *
 * This is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * This software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this package; see the file COPYING.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 *
 * $Id: config.c,v 1.18 2009-05-14 08:11:33 hanke Exp $
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <glib.h>

#include "speechd.h"
#include "configuration.h"

/* == DEFAULT OPTIONS == */

static void
spd_update_log_level(GSettings *settings,
					 gchar *key,
					 gpointer user_data)
{
	SpeechdOptions.log_level = g_settings_get_uint (settings, "log-level");
	MSG(0, "log level changing to %d", SpeechdOptions.log_level);
}

void load_default_global_set_options()
{
	spd_settings = g_settings_new ("org.freebsoft.speechd.server");
	GlobalFDSet.priority = g_settings_get_enum (spd_settings, "default-priority");
	GlobalFDSet.msg_settings.punctuation_mode = g_settings_get_enum (spd_settings, "default-punctuation-mode");
	GlobalFDSet.msg_settings.spelling_mode = g_settings_get_boolean (spd_settings, "default-spelling");
	GlobalFDSet.msg_settings.rate = g_settings_get_int (spd_settings, "default-rate");
	GlobalFDSet.msg_settings.pitch = g_settings_get_int (spd_settings, "default-pitch");
	GlobalFDSet.msg_settings.pitch_range = g_settings_get_int (spd_settings, "default-pitch-range");
	GlobalFDSet.msg_settings.volume = g_settings_get_int (spd_settings, "default-volume");
	GlobalFDSet.client_name = g_settings_get_string (spd_settings, "default-client-name");
	GlobalFDSet.msg_settings.voice.language = g_settings_get_string (spd_settings, "default-language");
	GlobalFDSet.output_module = g_settings_get_string (spd_settings, "default-module");
	GlobalFDSet.msg_settings.voice_type = g_settings_get_enum (spd_settings, "default-voice-type");
	GlobalFDSet.msg_settings.cap_let_recogn = g_settings_get_enum (spd_settings, "default-capital-letter-recognition");
	GlobalFDSet.min_delay_progress = 2000;
	GlobalFDSet.pause_context = g_settings_get_uint (spd_settings, "default-pause-context");
	GlobalFDSet.ssml_mode = SPD_DATA_TEXT;
	GlobalFDSet.notification = 0;

	GlobalFDSet.audio_output_method = g_settings_get_string (spd_settings, "audio-output-method");
	GlobalFDSet.audio_oss_device = g_settings_get_string (spd_settings, "audio-oss-device");
	GlobalFDSet.audio_alsa_device = g_settings_get_string (spd_settings, "audio-alsa-device");
	GlobalFDSet.audio_nas_server = g_settings_get_string (spd_settings, "audio-nas-server");
	GlobalFDSet.audio_pulse_server = g_settings_get_string (spd_settings, "audio-pulse-server");
	GlobalFDSet.audio_pulse_min_length = g_settings_get_uint (spd_settings, "audio-pulse-min-length");

	SpeechdOptions.max_history_messages = 10000;

	/* Options which are accessible from command line must be handled
	   specially to make sure we don't overwrite them */
	if (!SpeechdOptions.log_level_set) {
		g_signal_connect (spd_settings, "changed::log-level",
						  G_CALLBACK(spd_update_log_level), NULL);
		spd_update_log_level (spd_settings, NULL, NULL);
	}
	if (!SpeechdOptions.communication_method_set) {
		SpeechdOptions.communication_method = g_settings_get_enum (spd_settings, "communication-method");
	}
	if (!SpeechdOptions.socket_path_set)
		SpeechdOptions.socket_path = g_settings_get_string (spd_settings, "socket-path");
	if (!SpeechdOptions.port_set)
		SpeechdOptions.port = g_settings_get_uint (spd_settings, "port");
	if (!SpeechdOptions.localhost_access_only_set)
		SpeechdOptions.localhost_access_only = g_settings_get_boolean (spd_settings, "localhost-access-only");
	if (!SpeechdOptions.server_timeout_set)
		SpeechdOptions.server_timeout = g_settings_get_uint (spd_settings, "timeout");
	

	logfile = stderr;
	custom_logfile = NULL;
}
