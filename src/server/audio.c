
/*
 * spd_audio.c -- Spd Audio Output Library
 *
 * Copyright (C) 2004, 2006 Brailcom, o.p.s.
 *
 * This is free software; you can redistribute it and/or modify it under the
 * terms of the GNU Lesser General Public License as published by the Free
 * Software Foundation; either version 2.1, or (at your option) any later
 * version.
 *
 * This software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this package; see the file COPYING.  If not, write to the Free
 * Software Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 *
 * $Id: spd_audio.c,v 1.21 2008-06-09 10:29:12 hanke Exp $
 */

/*
 * spd_audio is a simple realtime audio output library with the capability of
 * playing 8 or 16 bit data, immediate stop and synchronization. This library
 * currently provides OSS, NAS, ALSA and PulseAudio backend. The available backends are
 * specified at compile-time using the directives WITH_OSS, WITH_NAS, WITH_ALSA,
 * WITH_PULSE, WITH_LIBAO but the user program is allowed to switch between them at run-time.
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include "audio.h"

#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>

#include <pthread.h>

#include <glib.h>
#include <glib/gstdio.h>
#include <ltdl.h>

#include "speechd.h"
#include "speechd_defines.h"
#include "set.h"

static int spd_audio_log_level;
static lt_dlhandle lt_h;

/* Server audio socket file descriptor */
static int audio_server_socket;

AudioID *audio_id;
static char *audio_pars[10];	/* Audio module parameters */

/* Audio thread mainloop */
static GMainContext* audio_thread_context = NULL;
static GMainLoop* audio_thread_loop = NULL;
static GSource* audio_socket_source = NULL;
static GSource* audio_thread_idle_source = NULL;
static gboolean audio_close_requested = FALSE;

/* A linked list of TAudioFDSetElement structures for modules */
static GQueue *module_data_list;

static void free_fd_set(gpointer data);
static void speechd_audio_cleanup(void);
static void set_audio_thread_attributes();

/* Dynamically load a library with RTLD_GLOBAL set.

   This is needed when a dynamically-loaded library has its own plugins
   that call into the parent library.
   Most of the credit for this function goes to Gary Vaughan.
*/
static lt_dlhandle my_dlopenextglobal(const char *filename)
{
	lt_dlhandle handle = NULL;
	lt_dladvise advise;

	if (lt_dladvise_init(&advise))
		return handle;

	if (!lt_dladvise_ext(&advise) && !lt_dladvise_global(&advise))
		handle = lt_dlopenadvise(filename, advise);

	lt_dladvise_destroy(&advise);
	return handle;
}

/* Open the audio device.

   Arguments:
   type -- The requested device. Currently AudioOSS or AudioNAS.
   pars -- and array of pointers to parameters to pass to
           the device backend, terminated by a NULL pointer.
           See the source/documentation of each specific backend.
   error -- a pointer to the string where error description is
           stored in case of failure (returned AudioID == NULL).
           Otherwise will contain NULL.

   Return value:
   Newly allocated AudioID structure that can be passed to
   all other spd_audio functions, or NULL in case of failure.

*/
AudioID *spd_audio_open(char *name, void **pars, char **error)
{
	MSG(5, "spd_audio_open called with name %s", name);
	AudioID *id;
	spd_audio_plugin_t const *p;
	spd_audio_plugin_t *(*fn) (void);
	gchar *libname;
	int ret;

	/* now check whether dynamic plugin is available */
	ret = lt_dlinit();
	if (ret != 0) {
		*error = (char *)g_strdup_printf("lt_dlinit() failed");
		return (AudioID *) NULL;
	}

	ret = lt_dlsetsearchpath(PLUGIN_DIR);
	if (ret != 0) {
		*error = (char *)g_strdup_printf("lt_dlsetsearchpath() failed");
		return (AudioID *) NULL;
	}

	libname = g_strdup_printf(SPD_AUDIO_LIB_PREFIX "%s", name);
	lt_h = my_dlopenextglobal(libname);
	g_free(libname);
	if (NULL == lt_h) {
		*error =
		    (char *)g_strdup_printf("Cannot open plugin %s. error: %s",
					    name, lt_dlerror());
		return (AudioID *) NULL;
	}

	fn = lt_dlsym(lt_h, SPD_AUDIO_PLUGIN_ENTRY_STR);
	if (NULL == fn) {
		*error = (char *)g_strdup_printf("Cannot find symbol %s",
						 SPD_AUDIO_PLUGIN_ENTRY_STR);
		return (AudioID *) NULL;
	}

	MSG(5, "calling init function");
	p = fn();
	if (p == NULL || p->name == NULL) {
		*error = (char *)g_strdup_printf("plugin %s not found", name);
		return (AudioID *) NULL;
	}

	MSG(5, "calling open function");
	id = p->open(pars);
	if (id == NULL) {
		*error =
		    (char *)g_strdup_printf("Couldn't open %s plugin", name);
		return (AudioID *) NULL;
	}

	id->function = p;
#if defined(BYTE_ORDER) && (BYTE_ORDER == BIG_ENDIAN)
	id->format = SPD_AUDIO_BE;
#else
	id->format = SPD_AUDIO_LE;
#endif

	*error = NULL;

	return id;
}

/* Play a track on the audio device (blocking).

   Arguments:
   id -- the AudioID* of the device returned by spd_audio_open
   track -- a track to play (see spd_audio.h)

   Return value:
   0 if everything is ok, a non-zero value in case of failure.
   See the particular backend documentation or source for the
   meaning of these non-zero values.

   Comment:
   spd_audio_play() is a blocking function. It returns exactly
   when the given track stopped playing. However, it's possible
   to safely interrupt it using spd_audio_stop() described below.
   (spd_audio_stop() needs to be called from another thread, obviously.)

*/
int spd_audio_play(AudioID * id, AudioTrack track, AudioFormat format)
{
	int ret;

	if (id && id->function->play) {
		/* Only perform byte swapping if the driver in use has given us audio in
		   an endian format other than what the running CPU supports. */
		if (format != id->format) {
			unsigned char *out_ptr, *out_end, c;
			out_ptr = (unsigned char *)track.samples;
			out_end =
			    out_ptr +
			    track.num_samples * 2 * track.num_channels;
			while (out_ptr < out_end) {
				c = out_ptr[0];
				out_ptr[0] = out_ptr[1];
				out_ptr[1] = c;
				out_ptr += 2;
			}
		}
		MSG(5, "playing audio on audio_id %d", id);
		ret = id->function->play(id, track);
	} else {
		fprintf(stderr, "Play not supported on this device\n");
		return -1;
	}

	return ret;
}

/* Stop playing the current track on device id

Arguments:
   id -- the AudioID* of the device returned by spd_audio_open

Return value:
   0 if everything is ok, a non-zero value in case of failure.
   See the particular backend documentation or source for the
   meaning of these non-zero values.

Comment:
   spd_audio_stop() safely interrupts spd_audio_play() when called
   from another thread. It shouldn't cause any clicks or unwanted
   effects in the sound output.

   It's safe to call spd_audio_stop() even if the device isn't playing
   any track. In that case, it does nothing. However, there is a danger
   when using spd_audio_stop(). Since you must obviously do it from
   another thread than where spd_audio_play is running, you must make
   yourself sure that the device is still open and the id you pass it
   is valid and will be valid until spd_audio_stop returns. In other words,
   you should use some mutex or other synchronization device to be sure
   spd_audio_close isn't called before or during spd_audio_stop execution.
*/

int spd_audio_stop(AudioID * id)
{
	int ret;
	if (id && id->function->stop) {
		ret = id->function->stop(id);
	} else {
		fprintf(stderr, "Stop not supported on this device\n");
		return -1;
	}
	return ret;
}

/* Close the audio device id

Arguments:
   id -- the AudioID* of the device returned by spd_audio_open

Return value:
   0 if everything is ok, a non-zero value in case of failure.

Comments:

   Please make sure no other spd_audio function with this device id
   is running in another threads. See spd_audio_stop() for detailed
   description of possible problems.
*/
int spd_audio_close(AudioID * id)
{
	int ret = 0;
	if (id && id->function->close) {
		ret = (id->function->close(id));
	}

	if (NULL != lt_h) {
		lt_dlclose(lt_h);
		lt_h = NULL;
		lt_dlexit();
	}

	return ret;
}

/* Set volume for playing tracks on the device id

Arguments:
   id -- the AudioID* of the device returned by spd_audio_open
   volume -- a value in the range <-100:100> where -100 means the
             least volume (probably silence), 0 the default volume
	     and +100 the highest volume possible to make on that
	     device for a single flow (i.e. not using mixer).

Return value:
   0 if everything is ok, a non-zero value in case of failure.
   See the particular backend documentation or source for the
   meaning of these non-zero values.

Comments:

   In case of /dev/dsp, it's not possible to set volume for
   the particular flow. For that reason, the value 0 means
   the volume the track was recorded on and each smaller value
   means less volume (since this works by deviding the samples
   in the track by a constant).
*/
int spd_audio_set_volume(AudioID * id, int volume)
{
	if ((volume > 100) || (volume < -100)) {
		fprintf(stderr, "Requested volume out of range");
		return -1;
	}
	if (id == NULL) {
		fprintf(stderr, "audio id is NULL in spd_audio_set_volume\n");
		return -1;
	}
	id->volume = volume;
	return 0;
}

void spd_audio_set_loglevel(AudioID * id, int level)
{
	if (level) {
		spd_audio_log_level = level;
		if (id != 0 && id->function != 0)
			id->function->set_loglevel(level);
	}
}

char const *spd_audio_get_playcmd(AudioID * id)
{
	if (id != 0 && id->function != 0) {
		return id->function->get_playcmd();
	}
	return NULL;
}

void speechd_audio_socket_init(void)
{
	/* For now use unix socket for audio. Maybe later we can add inet socket support */
	GString *audio_socket_filename;
	audio_socket_filename = g_string_new("");
	if (SpeechdOptions.runtime_speechd_dir) {
		g_string_printf(audio_socket_filename, "%s/audio.sock",
				SpeechdOptions.runtime_speechd_dir);
	} else {
		FATAL
		    ("Socket name file not set and user has no runtime directory");
	}
	g_free(SpeechdOptions.audio_socket_path);
	SpeechdOptions.audio_socket_path = g_strdup(audio_socket_filename->str);
	g_string_free(audio_socket_filename, 1);

	MSG(1, "Creating audio socket at %s", SpeechdOptions.audio_socket_path);

	/* Audio data is only using unix sockets for now, possibly adapt to use 
	 * inet sockets also later? */
	if (g_file_test(SpeechdOptions.audio_socket_path, G_FILE_TEST_EXISTS))
		if (g_unlink(SpeechdOptions.audio_socket_path) == -1)
			FATAL
			    ("Local socket file for audio exists but impossible to delete. Wrong permissions?");
	/* Connect and start listening on local unix socket */
	audio_server_socket =
	    make_local_socket(SpeechdOptions.audio_socket_path);
}

/* play the audio data on _fd_ if we got some activity. */
int play_audio(int fd)
{
	size_t bytes = 0;	/* Number of bytes we got */
	int buflen = BUF_SIZE;
	char *buf = (char *)g_malloc(buflen + 1);
	AudioTrack track;
	AudioFormat format;
	gchar **metadata;
	int bytes_read;

	/* Read data from socket */
	/* Read exactly one complete line, the `parse' routine relies on it */
	{
		while (1) {
			int n = read(fd, buf + bytes, 1);
			if (n <= 0) {
				MSG(5, "ERROR: Read 0 bytes from fd");
				g_free(buf);
				return -1;
			}
			/* Note, bytes is a 0-based index into buf. */
			if ((buf[bytes] == '\n')
			    && (bytes >= 1) && (buf[bytes - 1] == '\r')) {
				buf[++bytes] = '\0';
				break;
			}
			if (buf[bytes] == '\0')
				buf[bytes] = '?';
			if ((++bytes) == buflen) {
				buflen *= 2;
				buf = g_realloc(buf, buflen + 1);
			}
		}
	}

	/* Parse the data and read the reply */
	MSG2(5, "protocol", "%d:DATA:|%s| (%d)", fd, buf, bytes);
	if (strcmp(buf, "ACK" NEWLINE) == 0) {
		g_free(buf);
		return 0;
	}
	/* parse the AudioTrack information from buf */
	metadata = g_strsplit(buf, ":", 5);
	if (metadata == NULL || metadata[0] == NULL
	    || metadata[1] == NULL || metadata[2] == NULL
	    || metadata[3] == NULL || metadata[4] == NULL) {
		MSG(5, "Error: Unable to read Audiotrack metadata!");
		return -1;
	}
	format = strtol(metadata[0], NULL, 10);
	track.bits = strtol(metadata[1], NULL, 10);
	track.num_channels = strtol(metadata[2], NULL, 10);
	track.sample_rate = strtol(metadata[3], NULL, 10);
	track.num_samples = strtol(metadata[4], NULL, 10);

	MSG(5, "Track num samples is %d", track.num_samples);

	if (track.num_samples <= 0) {
		MSG(5, "Error: num_samples is invalid");
		return -1;
	}
	/* then free buf  */
	g_free(buf);
	/* Get the rest of the data */
	track.samples = g_malloc0_n(track.num_samples, sizeof(signed short));
	bytes_read =
	    read(fd, track.samples, track.num_samples * sizeof(signed short));

	if (bytes_read != track.num_samples * sizeof(signed short)) {
		MSG(5, "Error: num_samples %d doesn't match bytes read %d",
		    track.num_samples, bytes_read);
		return -1;
	}

	MSG(5, "Going to play audio on audio with id %d", audio_id);

	/* And play the AudioTrack */
	if (spd_audio_play(audio_id, track, format) < 0) {
		MSG(5, "Error: unable to play audio");
		return -1;
	}

	return 0;
}

static gboolean audio_socket_process_incoming(gpointer data)
{
	int ret;

	ret = speechd_audio_connection_new(audio_server_socket);
	if (ret != 0) {
		MSG(2, "Error: Failed to add new module audio!");
		if (SPEECHD_DEBUG) {
			FATAL("Failed to add new module audio!");
		}
	}

	return TRUE;
}

static gboolean audio_process_incoming(int      fd,
				       gpointer null, 
                                       gpointer user_data)
{
	TAudioFDSetElement *fd_set = (TAudioFDSetElement *)user_data;

	MSG(5, "audio_process_incoming called for fd %d", fd_set->fd);
	int nread;

	ioctl(fd_set->fd, FIONREAD, &nread);

	if (nread == 0) {
		/* module has gone */
		MSG(2, "Info: Module has gone.");
		g_queue_remove(module_data_list, fd_set);
		free_fd_set(fd_set);
		return FALSE;
	}

	MSG(5, "read %d bytes from fd %d", nread, fd_set->fd);

	/* client sends some commands or data */
	if (play_audio(fd_set->fd) == -1) {
		MSG(2, "Error: Failed to serve client on fd %d!", fd_set->fd);
	}

	return TRUE;
}

static gboolean check_close_requested (gpointer data)
{
	pthread_mutex_lock(&audio_close_mutex);
	if (audio_close_requested) {
		pthread_mutex_unlock(&audio_close_mutex);
		g_main_loop_quit(audio_thread_loop);
		return FALSE;
	}

	pthread_mutex_unlock(&audio_close_mutex);
	return TRUE;
}

/* Playback thread. */
void *_speechd_play(void *nothing)
{
	char *error = 0;
	gchar **outputs;
	int i = 0;
	gboolean found_audio_module = FALSE;

	MSG(1, "Playback thread starting.......");

	module_data_list = g_queue_new();

	/* TODO: Use real values from config rather than these hard coded test values */
	if (GlobalFDSet.audio_oss_device != NULL)
		audio_pars[1] = g_strdup(GlobalFDSet.audio_oss_device);
	else
		audio_pars[1] = NULL;

	if (GlobalFDSet.audio_alsa_device != NULL)
		audio_pars[2] = g_strdup(GlobalFDSet.audio_alsa_device);
	else
		audio_pars[2] = NULL;

	if (GlobalFDSet.audio_nas_server != NULL)
		audio_pars[3] = g_strdup(GlobalFDSet.audio_nas_server);
	else
		audio_pars[3] = NULL;

	if (GlobalFDSet.audio_pulse_server != NULL)
		audio_pars[4] = g_strdup(GlobalFDSet.audio_pulse_server);
	else
		audio_pars[4] = NULL;

	if (GlobalFDSet.audio_pulse_min_length > 9)
		audio_pars[5] =
		    g_strdup_printf("%d", GlobalFDSet.audio_pulse_min_length);
	else
		audio_pars[5] = NULL;

	MSG(1, "Openning audio output system");
	if (GlobalFDSet.audio_output_method == NULL) {
		MSG(1,
		    "Sound output method specified in configuration not supported. "
		    "Please choose 'oss', 'alsa', 'nas', 'libao' or 'pulse'.");
		return 0;
	}

	outputs = g_strsplit(GlobalFDSet.audio_output_method, ",", 0);
	while (NULL != outputs[i]) {
		audio_id =
		    spd_audio_open(outputs[i], (void **)&audio_pars[1], &error);
		if (audio_id) {
			spd_audio_set_loglevel(audio_id,
					       SpeechdOptions.log_level);
			MSG(5, "Using %s audio output method with log level %d",
			    outputs[i], SpeechdOptions.log_level);

			/* Volume is controlled by the synthesizer. Always play at normal on audio device. */
			if (spd_audio_set_volume(audio_id, 85) < 0) {
				MSG(2,
				    "Can't set volume. audio not initialized?");
			}

			g_strfreev(outputs);
			MSG(5, "audio initialized successfully.");
			found_audio_module = TRUE;
			break;
		}
		i++;
	}

	if (!found_audio_module) {
		MSG(1, "Opening sound device failed. Reason: %s. ", error);
		g_free(error);	/* g_malloc'ed, in spd_audio_open. */
	}

	/* Create the audio thread main context and loop */
	audio_thread_context = g_main_context_new();
	audio_thread_loop = g_main_loop_new(audio_thread_context, FALSE);

	audio_socket_source = g_unix_fd_source_new(audio_server_socket,
				                   G_IO_IN);
	g_source_set_callback(audio_socket_source,
			      audio_socket_process_incoming,
			      NULL, NULL);
	g_source_attach(audio_socket_source, audio_thread_context);

	audio_thread_idle_source = g_idle_source_new();
	g_source_set_callback(audio_thread_idle_source, check_close_requested, NULL, NULL);
	g_source_attach(audio_thread_idle_source, audio_thread_context);

	/* Block all signals to this thread. */
	set_audio_thread_attributes();

	g_main_loop_run(audio_thread_loop);

	MSG(1, "Playback thread stopping.");

	speechd_audio_cleanup();

	g_source_destroy(audio_socket_source);
	g_source_unref(audio_socket_source);
	audio_socket_source = NULL;

	g_source_destroy(audio_thread_idle_source);
	g_source_unref(audio_thread_idle_source);
	audio_thread_idle_source = NULL;

	/*
	 * Close the module descriptors, destroy sources, and free the fd_set
	 * structure.
	 */
	g_queue_free_full(module_data_list, free_fd_set);

	g_main_loop_unref(audio_thread_loop);
	g_main_context_unref(audio_thread_context);

	MSG(1, "Playback thread ended.......");
	return 0;
}

/* activity is on audio_server_socket (request for a new connection) */
int speechd_audio_connection_new(int audio_server_socket)
{
	MSG(5, "Adding audio connection on socket %d", audio_server_socket);
	TAudioFDSetElement *new_fd_set;
	struct sockaddr_in module_address;
	unsigned int module_len = sizeof(module_address);
	int module_socket;

	module_socket =
	    accept(audio_server_socket, (struct sockaddr *)&module_address,
		   &module_len);

	if (module_socket == -1) {
		MSG(2,
		    "Error: Can't handle connection request of a module for audio");
		return -1;
	}

	MSG(4, "Adding module on fd %d", module_socket);

	/* Create a record in fd_settings */
	new_fd_set = (TAudioFDSetElement *) default_audio_fd_set();
	if (new_fd_set == NULL) {
		MSG(2,
		    "Error: Failed to create a record in fd_settings for the module for audio");
		return -1;
	}

	new_fd_set->fd = module_socket;
	new_fd_set->source = g_unix_fd_source_new(module_socket, G_IO_IN);
	g_source_set_callback(new_fd_set->source,
			      (GSourceFunc)audio_process_incoming,
			      new_fd_set, NULL);
	g_source_attach(new_fd_set->source, audio_thread_context);
	g_queue_push_tail(module_data_list, new_fd_set);

	return 0;
}

static void speechd_audio_cleanup(void)
{
	if (close(audio_server_socket) == -1)
		MSG(2, "close() audio server socket failed: %s",
		    strerror(errno));

	MSG(2, "Closing audio output...");
	spd_audio_close(audio_id);
	audio_id = NULL;
}

/*
 * This is currently the same as the similarly named function in speaking.c
 * but we have no need to pull in everything else from speaking.h, and there
 * may be a reason to change this function at a later date.
 */
static void set_audio_thread_attributes()
{
	int ret;
	sigset_t all_signals;

	ret = sigfillset(&all_signals);
	if (ret == 0) {
		ret = pthread_sigmask(SIG_BLOCK, &all_signals, NULL);
		if (ret != 0)
			MSG(1,
			    "Can't set signal set, expect problems when terminating!");
	} else {
		MSG(1,
		    "Can't fill signal set, expect problems when terminating!");
	}

	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
}

static void free_fd_set(gpointer data)
{
	TAudioFDSetElement *fd_set = (TAudioFDSetElement *)data;

	if (fd_set != NULL) {
		close(fd_set->fd);
		if (fd_set->output_module != NULL)
			g_free(fd_set->output_module);
		if (fd_set->source != NULL) {
			g_source_destroy(fd_set->source);
			g_source_unref(fd_set->source);
		}
		g_free(fd_set);
	}
}

void close_audio_thread(void)
{
	pthread_mutex_lock(&audio_close_mutex);
	audio_close_requested = TRUE;
	pthread_mutex_unlock(&audio_close_mutex);
}
