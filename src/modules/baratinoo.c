/*
 * baratinoo.c - Speech Dispatcher backend for Baratinoo (VoxyGen)
 *
 * Copyright (C) 2016 Brailcom, o.p.s.
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
 */

/*
 * Input and output choices.
 *
 * - The input is sent to the engine through a BCinputTextBuffer.  There is
 *   a single one of those at any given time, and it is filled in
 *   module_speak() and consumed in the synthesis thread.
 *
 *   This doesn't use an input callback generating a continuous flow (and
 *   blocking waiting for more data) even though it would be a fairly nice
 *   design and would allow not to set speech attributes like volume, pitch and
 *   rate as often.  This is because the Baratinoo engine has 2 limitations on
 *   the input callback:
 *
 *   * It consumes everything (or at least a lot) up until the callbacks
 *     reports the input end by returning 0.  Alternatively one could use the
 *     \flush command followed by a newline, so this is not really limiting.
 *
 *   * More problematic, as the buffer callback is expected to feed a single
 *     input, calling BCpurge() (for handling stop events) unregisters it,
 *     requiring to re-add it afterward.  This renders the continuous flow a
 *     lot less useful, as speech attributes like volume, pitch and rate would
 *     have to be set again.
 *
 * - The output uses the output callback.  This is both simple and efficient,
 *   as there is no intermediate representation of the sound data, and no copy.
 *   The data is directly fed to the output module without further computation.
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <semaphore.h>

#define BARATINOO_C_API
#include "baratinoo.h"
#include "baratinooio.h"

#include "spd_audio.h"

#include <speechd_types.h>

#include "module_utils.h"

#define MODULE_NAME     "baratinoo"
#define DBG_MODNAME     "Baratinoo:"
#define MODULE_VERSION  "0.1"

#define DEBUG_MODULE 1
DECLARE_DEBUG();

/* Thread and process control */
static SPDVoice **baratinoo_voice_list = NULL;

gboolean baratinoo_stop_requested = FALSE;
gboolean baratinoo_close_requested = FALSE;

static pthread_t baratinoo_speak_thread;
static sem_t baratinoo_semaphore;

static BCengine baratinoo_engine = NULL;
static BCinputTextBuffer baratinoo_text_buffer = NULL;
static int baratinoo_voice = 0;

/* Internal functions prototypes */
static void *_baratinoo_speak(void *);

MOD_OPTION_1_STR(BaratinooConfigPath);

/* Public functions */

int module_load(void)
{
	const char *conf_env;
	char *default_config = NULL;

	INIT_SETTINGS_TABLES();

	REGISTER_DEBUG();

	conf_env = getenv("BARATINOO_CONFIG_PATH");
	if (conf_env && conf_env[0] != '\0') {
		default_config = g_strdup(conf_env);
	} else {
		default_config = g_build_filename(g_get_user_config_dir(),
						  "baratinoo.cfg", NULL);
	}
	MOD_OPTION_1_STR_REG(BaratinooConfigPath, default_config);
	g_free(default_config);

	return 0;
}

void baratinoo_trace_cb(BaratinooTraceLevel level, int engine_num, const char *source, const void *data, const char *format, va_list args)
{
	const char *prefix = "";

	switch(level) {
	case BARATINOO_TRACE_ERROR:
		prefix = "ERROR";
		break;
	case BARATINOO_TRACE_INIT:
		prefix = "INIT";
		break;
	case BARATINOO_TRACE_WARNING:
		prefix = "WARNING";
		break;
	case BARATINOO_TRACE_INFO:
		prefix = "INFO";
		break;
	case BARATINOO_TRACE_DEBUG:
		prefix = "DEBUG";
		break;
	}

	fprintf(stderr, "%s: %s ", prefix, source);
	vfprintf(stderr, format, args);
	fprintf(stderr, "\n");
}

static int baratinoo_output_signal_cb(void *privateData, const void *address, int length)
{
	AudioTrack track;

	if (baratinoo_stop_requested) {
		DBG("Not playing message because it got stopped");
		return 0;
	}

	track.num_samples = length / 2; /* 16 bits per sample = 2 bytes */
	track.num_channels = 1;
	track.sample_rate = 16000;
	track.bits = 16;
	track.samples = (short *) address;

	DBG("Playing part of the message");
	if (module_tts_output(track, SPD_AUDIO_LE) < 0)
		DBG("ERROR: failed to play the track");

	return 0;
}

int module_init(char **status_info)
{
	int ret;
	BARATINOOC_STATE state;
	int n_voices;
	int i;

	DBG("Module init");
	INIT_INDEX_MARKING();

	*status_info = NULL;

	baratinoo_stop_requested = FALSE;
	baratinoo_close_requested = FALSE;

	/* Init Baratinoo */
	if (BCinitlib(baratinoo_trace_cb) != BARATINOO_INIT_OK) {
		DBG("Failed to initialize Baratinoo");
		*status_info = g_strdup("Failed to initialize Baratinoo. "
					"Make sure your installation is "
					"properly st up.");
		return -1;
	}
	DBG("Using Baratinoo %s", BCgetBaratinooVersion());

	baratinoo_engine = BCnew(NULL);
	if (!baratinoo_engine) {
		DBG("Failed to allocate engine");
		*status_info = g_strdup("Failed to create Baratinoo engine.");

		BCterminatelib();
		return -1;
	}

	BCinit(baratinoo_engine, BaratinooConfigPath);
	state = BCgetState(baratinoo_engine);
	if (state != BARATINOO_INITIALIZED) {
		DBG("Failed to initialize Baratinoo engine");
		*status_info = g_strdup("Failed to initialize Baratinoo engine. "
					"Make sure your setup is OK.");

		BCdelete(baratinoo_engine);
		BCterminatelib();
		return -1;
	}

	/* Find voices */
	n_voices = BCgetNumberOfVoices(baratinoo_engine);
	if (n_voices < 1) {
		DBG("No voice available");
		*status_info = g_strdup("No Baratinoo voices found. "
					"Make sure your setup is OK.");

		BCdelete(baratinoo_engine);
		BCterminatelib();
		return -1;
	}
	baratinoo_voice_list = g_malloc_n(n_voices + 1, sizeof *baratinoo_voice_list);
	DBG("Got %d available voices:", n_voices);
	for (i = 0; i < n_voices; i++) {
		SPDVoice *voice;
		BaratinooVoiceInfo voice_info = BCgetVoiceInfo(baratinoo_engine, i);

		DBG("\tVoice #%d: name=%s, language=%s, gender=%s",
		    i, voice_info.name, voice_info.language, voice_info.gender);

		voice = g_malloc0(sizeof *voice);
		voice->name = g_strdup(voice_info.name);
		// FIXME: check the format
		voice->language = g_strndup(voice_info.iso639, 2);
		voice->variant = g_strdup(voice_info.variant);

		baratinoo_voice_list[i] = voice;
	}
	baratinoo_voice_list[i] = NULL;

	BCsetOutputSignal(baratinoo_engine, baratinoo_output_signal_cb,
			  NULL, BARATINOO_PCM, 16000 /* default frequency */);

	sem_init(&baratinoo_semaphore, 0, 0);

	DBG("Baratinoo: creating new thread for baratinoo_speak\n");
	ret = pthread_create(&baratinoo_speak_thread, NULL, _baratinoo_speak, NULL);
	if (ret != 0) {
		BCdelete(baratinoo_engine);
		BCterminatelib();

		DBG("Baratinoo: thread failed\n");
		*status_info =
		    g_strdup("The module couldn't initialize threads "
			     "This could be either an internal problem or an "
			     "architecture problem. If you are sure your architecture "
			     "supports threads, please report a bug.");
		return -1;
	}

	DBG("Baratinoo initialized successfully.");
	*status_info = g_strdup("Baratinoo initialized successfully.");

	return 0;
}

SPDVoice **module_list_voices(void)
{
	return baratinoo_voice_list;
}

/* return: <0 @a is best, >0 @b is best */
static int sort_voice(const BaratinooVoiceInfo *a, const BaratinooVoiceInfo *b, const char *lang, SPDVoiceType voice_code)
{
	int cmp = 0;

	if (strcmp(lang, a->iso639) == 0)
		cmp--;
	if (strcmp(lang, b->iso639) == 0)
		cmp++;

	if (strcmp(a->gender, b->gender) != 0) {
		const char *gender;

		switch (voice_code) {
		default:
		case SPD_MALE1:
		case SPD_MALE2:
		case SPD_MALE3:
		case SPD_CHILD_MALE:
			gender = "male";
			break;

		case SPD_FEMALE1:
		case SPD_FEMALE2:
		case SPD_FEMALE3:
		case SPD_CHILD_FEMALE:
			gender = "female";
			break;
		}

		if (strcmp(gender, a->gender) == 0)
			cmp--;
		if (strcmp(gender, b->gender) == 0)
			cmp++;
	}

	switch (voice_code) {
	case SPD_CHILD_MALE:
	case SPD_CHILD_FEMALE:
		if (a->age && a->age <= 15)
			cmp--;
		if (b->age && b->age <= 15)
			cmp++;
		break;
	default:
		/* we expect mostly adult voices, so only compare if age is set */
		if (a->age && b->age) {
			if (a->age > 15)
				cmp--;
			if (b->age > 15)
				cmp++;
		}
		break;
	}

	DBG(DBG_MODNAME " Comparing %s <> %s gives %d", a->name, b->name, cmp);

	return cmp;
}

/* Given a language code and SD voice code, sets the espeak voice. */
static void baratinoo_set_language_and_voice(char *lang, SPDVoiceType voice_code)
{
	int i;
	int best_match = -1;
	int nth_match = 0;
	int offset = 0; // nth voice we'd like
	BaratinooVoiceInfo best_info;

	DBG(DBG_MODNAME " set_language_and_voice %s %d", lang, voice_code);

	switch (voice_code) {
	case SPD_MALE3:
	case SPD_FEMALE3:
		offset++;
	case SPD_MALE2:
	case SPD_FEMALE2:
		offset++;
	default:
		break;
	}

	// FIXME: thread safety accessing @baratinoo_engine
	for (i = 0; i < BCgetNumberOfVoices(baratinoo_engine); i++) {
		if (i == 0) {
			best_match = i;
			best_info = BCgetVoiceInfo(baratinoo_engine, i);
			nth_match++;
		} else {
			BaratinooVoiceInfo info = BCgetVoiceInfo(baratinoo_engine, i);
			int cmp = sort_voice(&best_info, &info, lang, voice_code);

			if (cmp >= 0) {
				if (cmp > 0)
					nth_match = 0;
				if (nth_match <= offset) {
					best_match = i;
					best_info = info;
				}
				nth_match++;
			}
		}
	}

	if (best_match < 0) {
		DBG("No voice match found, not changing voice.");
	} else {
		DBG("Best voice match is %d.", best_match);
		baratinoo_voice = best_match;
	}
}

static void baratinoo_set_voice(SPDVoiceType voice)
{
	assert(msg_settings.voice.language);
	baratinoo_set_language_and_voice(msg_settings.voice.language, voice);
}

static void baratinoo_set_language(char *lang)
{
	baratinoo_set_language_and_voice(lang, msg_settings.voice_type);
}

static void baratinoo_set_synthesis_voice(char *synthesis_voice)
{
	int i;

	if (synthesis_voice == NULL)
		return;

	// FIXME: thread safety accessing @baratinoo_engine
	for (i = 0; i < BCgetNumberOfVoices(baratinoo_engine); i++) {
		BaratinooVoiceInfo info = BCgetVoiceInfo(baratinoo_engine, i);

		if (strcmp(synthesis_voice, info.name) == 0) {
			baratinoo_voice = i;
			return;
		}
	}

	DBG(DBG_MODNAME " Failed to set synthesis voice to %s.", synthesis_voice);
}

static gboolean baratinoo_speaking(void)
{
	return baratinoo_text_buffer != NULL;
}

/* locates a string in a NULL-terminated array of strings
 * Returns -1 if not found, the index otherwise. */
static int attribute_index(const char **names, const char *name)
{
	int i;

	for (i = 0; names && names[i] != NULL; i++) {
		if (strcmp(names[i], name) == 0)
			return i;
	}

	return -1;
}

static void ssml2baratinoo_start_element(GMarkupParseContext *ctx,
					 const gchar *element,
					 const gchar **attribute_names,
					 const gchar **attribute_values,
					 gpointer buffer, GError **error)
{
	if (strcmp(element, "mark") == 0) {
		int i = attribute_index(attribute_names, "name");
		g_string_append_printf(buffer, "\\mark{%s}",
				       i < 0 ? "" : attribute_values[i]);
	} else if (strcmp(element, "emphasis") == 0) {
		int i = attribute_index(attribute_names, "level");
		g_string_append_printf(buffer, "\\emph<{%s}",
				       i < 0 ? "" : attribute_values[i]);
	} else {
		/* ignore other elements */
		/* TODO: handle more elements */
	}
}

static void ssml2baratinoo_end_element(GMarkupParseContext *ctx,
				       const gchar *element,
				       gpointer buffer, GError **error)
{
	if (strcmp(element, "emphasis") == 0) {
		g_string_append(buffer, "\\emph>{}");
	}
}

static void ssml2baratinoo_text(GMarkupParseContext *ctx,
				const gchar *text, gsize len,
				gpointer buffer, GError **error)
{
	gsize i;

	for (i = 0; i < len; i++) {
		if (text[i] == '\\') {
			/* escape the \ by appending a comment so it won't be
			 * interpreted as a command */
			g_string_append(buffer, "\\\\{}");
		} else {
			g_string_append_c(buffer, text[i]);
		}
	}
}

static void append_ssml_as_proprietary(GString *buf, const char *data, gsize size)
{
	/* FIXME: we could possibly use SSML mode, but the Baratinoo parser is
	 * very strict and *requires* "xmlns", "version" and "lang" attributes
	 * on the <speak> tag, which speech-dispatcher doesn't provide.
	 *
	 * Moreover, we need to add tags for volume/rate/pitch so we'd have to
	 * amend the data anyway. */
	static const GMarkupParser parser = {
		.start_element = ssml2baratinoo_start_element,
		.end_element = ssml2baratinoo_end_element,
		.text = ssml2baratinoo_text,
	};
	GMarkupParseContext *ctx;
	GError *err = NULL;

	ctx = g_markup_parse_context_new(&parser, G_MARKUP_TREAT_CDATA_AS_TEXT,
					 buf, NULL);
	if (!g_markup_parse_context_parse(ctx, data, size, &err) ||
	    !g_markup_parse_context_end_parse(ctx, &err)) {
		DBG(DBG_MODNAME " Failed to convert SSML: %s", err->message);
		g_error_free(err);
	}

	g_markup_parse_context_free(ctx);
}

int module_speak(gchar *data, size_t bytes, SPDMessageType msgtype)
{
	GString *buffer = NULL;

	DBG("write()\n");

	assert(msg_settings.rate >= -100 && msg_settings.rate <= +100);
	assert(msg_settings.pitch >= -100 && msg_settings.pitch <= +100);
	assert(msg_settings.pitch_range >= -100 && msg_settings.pitch_range <= +100);
	assert(msg_settings.volume >= -100 && msg_settings.volume <= +100);

	if (baratinoo_speaking()) {
		// FIXME: append to a queue?
		DBG("Speaking when requested to write");
		return 0;
	}

	baratinoo_stop_requested = FALSE;

	/* select voice following parameters.  we don't use tags for this as
	 * we need to do some computation on our end anyway and need pass an
	 * ID when creating the buffer too */
	UPDATE_STRING_PARAMETER(voice.language, baratinoo_set_language);
	UPDATE_PARAMETER(voice_type, baratinoo_set_voice);
	UPDATE_STRING_PARAMETER(voice.name, baratinoo_set_synthesis_voice);

	baratinoo_text_buffer = BCinputTextBufferNew(BARATINOO_PROPRIETARY_PARSING,
						     BARATINOO_UTF8,
						     baratinoo_voice, 0);
	if (! baratinoo_text_buffer) {
		DBG("Failed to allocate input buffer");
		goto err;
	}

	buffer = g_string_new(NULL);

	/* Apply speech parameters */
	if (msg_settings.rate != 0) {
		g_string_append_printf(buffer, "\\rate{%+d%%}\n",
				       msg_settings.rate);
	}
	if (msg_settings.pitch != 0 || msg_settings.pitch_range != 0) {
		g_string_append_printf(buffer, "\\pitch{%+d%% %+d%%}\n",
				       msg_settings.pitch,
				       msg_settings.pitch_range);
	}
	if (msg_settings.volume != 0) {
		g_string_append_printf(buffer, "\\volume{%+d%%}\n",
				       msg_settings.volume);
	}

	switch (msgtype) {
	case SPD_MSGTYPE_SPELL: /* FIXME: use \spell one day? */
	case SPD_MSGTYPE_CHAR:
		g_string_append(buffer, "\\sayas<{characters}");
		append_ssml_as_proprietary(buffer, data, bytes);
		g_string_append(buffer, "\\sayas>");
		break;
	default: /* FIXME: */
	case SPD_MSGTYPE_TEXT:
		append_ssml_as_proprietary(buffer, data, bytes);
		break;
	}

	DBG(DBG_MODNAME " Sending buffer: %s", buffer->str);
	if (!BCinputTextBufferInit(baratinoo_text_buffer, buffer->str)) {
		DBG("Failed to initialize input buffer");
		goto err;
	}

	g_string_free(buffer, TRUE);

	sem_post(&baratinoo_semaphore);

	DBG(DBG_MODNAME " leaving write() normally");
	return bytes;

err:
	if (buffer)
		g_string_free(buffer, TRUE);
	if (baratinoo_text_buffer)
		BCinputTextBufferDelete(baratinoo_text_buffer);

	return 0;
}

int module_stop(void)
{
	DBG(DBG_MODNAME " stop()");
	if (!baratinoo_stop_requested) {
		baratinoo_stop_requested = TRUE;
		sem_post(&baratinoo_semaphore);
	}

	return 0;
}

size_t module_pause(void)
{
	// FIXME: ?
	DBG(DBG_MODNAME " Pause requested");
	if (baratinoo_speaking()) {
		DBG(DBG_MODNAME " Pause not supported, stopping");

		module_stop();

		return -1;
	} else {
		return 0;
	}
}

int module_close(void)
{
	DBG(DBG_MODNAME " close()");

	DBG("Terminating threads");

	baratinoo_stop_requested = TRUE;
	baratinoo_close_requested = TRUE;
	sem_post(&baratinoo_semaphore);
	/* Give threads a chance to quit on their own terms. */
	g_usleep(25000);

	/* Make sure threads have really exited */
	pthread_cancel(baratinoo_speak_thread);
	DBG("Joining threads.");
	if (pthread_join(baratinoo_speak_thread, NULL) != 0)
		DBG("Failed to join threads.");

	/* destroy voice list */
	if (baratinoo_voice_list != NULL) {
		int i;
		for (i = 0; baratinoo_voice_list[i] != NULL; i++) {
			g_free(baratinoo_voice_list[i]->name);
			g_free(baratinoo_voice_list[i]->language);
			g_free(baratinoo_voice_list[i]->variant);
			g_free(baratinoo_voice_list[i]);
		}
		g_free(baratinoo_voice_list);
		baratinoo_voice_list = NULL;
	}

	/* destroy engine */
	if (baratinoo_engine) {
		BCdelete(baratinoo_engine);
		baratinoo_engine = NULL;
	}

	/* uninitialize */
	BCterminatelib();

	sem_destroy(&baratinoo_semaphore);

	return 0;
}

/* Internal functions */

void *_baratinoo_speak(void *nothing)
{
	BARATINOOC_STATE state;

	set_speaking_thread_parameters();

	while (!baratinoo_close_requested) {
		sem_wait(&baratinoo_semaphore);
		DBG("Semaphore on");

		if (!baratinoo_text_buffer)
			continue;

		state = BCinputTextBufferSetInEngine(baratinoo_text_buffer,
						     baratinoo_engine);
		if (state != BARATINOO_READY) {
			DBG("Failed to set input buffer");
			continue;
		}

		module_report_event_begin();
		do {
			if (baratinoo_close_requested)
				break;
			else if (baratinoo_stop_requested) {
				state = BCpurge(baratinoo_engine);
				baratinoo_stop_requested = FALSE;
			}
			else
				state = BCprocessLoop(baratinoo_engine, 100);
			if (state == BARATINOO_EVENT) {
				/*BaratinooEvent ttsEvent = BCgetEvent(engine);*/
				DBG("Received an event");
			}
		} while (state == BARATINOO_RUNNING || state == BARATINOO_EVENT);
		DBG("leaving TTS loop state=%d", state);

		module_report_event_end();

		BCinputTextBufferDelete(baratinoo_text_buffer);
		baratinoo_text_buffer = NULL;
	}

	DBG("leaving thread with state=%d", state);

	return NULL;
}
