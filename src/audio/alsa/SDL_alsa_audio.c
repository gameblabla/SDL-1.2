/*
    SDL - Simple DirectMedia Layer
    Copyright (C) 1997-2012 Sam Lantinga

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Lesser General Public
    License as published by the Free Software Foundation; either
    version 2.1 of the License, or (at your option) any later version.

    This library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public
    License along with this library; if not, write to the Free Software
    Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA

    Sam Lantinga
    slouken@libsdl.org

    Modified in Oct 2004 by Hannu Savolainen 
    hannu@opensound.com
*/
#include "SDL_config.h"

/* Allow access to a raw mixing buffer */

#include <stdio.h>	/* For perror() */
#include <string.h>	/* For strerror() */
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/time.h>
#include <sys/ioctl.h>
#include <sys/stat.h>

#include <tinyalsa/pcm.h>

#include "SDL_timer.h"
#include "SDL_audio.h"
#include "../SDL_audiomem.h"
#include "../SDL_audio_c.h"
#include "../SDL_audiodev_c.h"
#include "SDL_alsa_audio.h"

#ifdef SDL_AUDIO_DRIVER_ALSA_DYNAMIC
#include "SDL_name.h"
#include "SDL_loadso.h"
#else
#define SDL_NAME(X)	X
#endif

/* The tag name used by DSP audio */
#define ALSA_DRIVER_NAME         "alsa"

/* Audio driver functions */
static int ALSA_OpenAudio(_THIS, SDL_AudioSpec *spec);
static void ALSA_WaitAudio(_THIS);
static void ALSA_PlayAudio(_THIS);
static Uint8 *ALSA_GetAudioBuf(_THIS);
static void ALSA_CloseAudio(_THIS);
/* Audio driver bootstrap functions */

#ifdef SDL_AUDIO_DRIVER_ALSA_DYNAMIC

static const char *alsa_library = SDL_AUDIO_DRIVER_ALSA_DYNAMIC;
static void *alsa_handle = NULL;
static int alsa_loaded = 0;

static struct pcm * (*SDL_NAME(pcm_open))(unsigned int card,
                     unsigned int device,
                     unsigned int flags,
                     const struct pcm_config *c);
static int (*SDL_NAME(pcm_close))(struct pcm *pcm);
static int (*SDL_NAME(pcm_writei))( struct pcm *pcm, const void *data, unsigned int frame_count);
static struct {
	const char *name;
	void **func;
} alsa_functions[] = {
	{ "pcm_open",	(void **)&SDL_NAME(pcm_open)	},
	{ "pcm_close",	(void **)&SDL_NAME(pcm_close)		},
	{ "pcm_writei",	(void **)&SDL_NAME(pcm_writei)	},
};

static void UnloadALSALibrary()
{
	if ( alsa_loaded ) {
		SDL_UnloadObject(alsa_handle);
		alsa_handle = NULL;
		alsa_loaded = 0;
	}
}

static int LoadALSALibrary(void)
{
	int i, retval = -1;

	alsa_handle = SDL_LoadObject(alsa_library);
	if ( alsa_handle ) {
		alsa_loaded = 1;
		retval = 0;
		for ( i=0; i<SDL_arraysize(alsa_functions); ++i ) {
			*alsa_functions[i].func = SDL_LoadFunction(alsa_handle, alsa_functions[i].name);
			if ( !*alsa_functions[i].func ) {
				retval = -1;
				UnloadALSALibrary();
				break;
			}
		}
	}
	return retval;
}

#else

static void UnloadALSALibrary()
{
	return;
}

static int LoadALSALibrary(void)
{
	return 0;
}

#endif /* SDL_AUDIO_DRIVER_ALSA_DYNAMIC */

static int Audio_Available(void)
{
	struct pcm *pcm_out_av;
	int available;

    struct pcm_config config_av = {
        .channels = 2,
        .rate = 44100,
        .format = PCM_FORMAT_S16_LE,
        .period_size = 2048
    };
    
    config_av.period_count = 2;
    config_av.start_threshold = config_av.period_size;
    config_av.silence_threshold = config_av.period_size * 2;
    config_av.stop_threshold = config_av.period_size * 2;

	available = 0;
	if ( LoadALSALibrary() < 0 ) {
		printf("Can't load Shared library\n");
		return available;
	}
	pcm_out_av = SDL_NAME(pcm_open)(0, 0, PCM_OUT, &config_av);
	if (pcm_out_av) {
		available = 1;
		SDL_NAME(pcm_close)(pcm_out_av);
	}
	printf("Unload Shared library\n");
	UnloadALSALibrary();
	return(available);
}

static void Audio_DeleteDevice(SDL_AudioDevice *device)
{
	SDL_free(device->hidden);
	SDL_free(device);
}

static SDL_AudioDevice *Audio_CreateDevice(int devindex)
{
	SDL_AudioDevice *this;

	/* Initialize all variables that we clean on shutdown */
	this = (SDL_AudioDevice *)SDL_malloc(sizeof(SDL_AudioDevice));
	if ( this ) {
		SDL_memset(this, 0, (sizeof *this));
		this->hidden = (struct SDL_PrivateAudioData *)
				SDL_malloc((sizeof *this->hidden));
	}
	if ( (this == NULL) || (this->hidden == NULL) ) {
		SDL_OutOfMemory();
		if ( this ) {
			SDL_free(this);
		}
		return(0);
	}
	SDL_memset(this->hidden, 0, (sizeof *this->hidden));

	/* Set the function pointers */
	this->OpenAudio = ALSA_OpenAudio;
	this->WaitAudio = ALSA_WaitAudio;
	this->PlayAudio = ALSA_PlayAudio;
	this->GetAudioBuf = ALSA_GetAudioBuf;
	this->CloseAudio = ALSA_CloseAudio;

	this->free = Audio_DeleteDevice;

	return this;
}

AudioBootStrap ALSA_bootstrap = {
	ALSA_DRIVER_NAME, "TinyALSA audio",
	Audio_Available, Audio_CreateDevice
};

/* This function waits until it is possible to write a full sound buffer */
static void ALSA_WaitAudio(_THIS)
{
	Sint32 ticks;

	/* Check to see if the thread-parent process is still alive */
	{ static int cnt = 0;
		/* Note that this only works with thread implementations 
		   that use a different process id for each thread.
		*/
		if (parent && (((++cnt)%10) == 0)) { /* Check every 10 loops */
			if ( kill(parent, 0) < 0 ) {
				this->enabled = 0;
			}
		}
	}
	
	/* Use timer for general audio synchronization */
	ticks = ((Sint32)(next_frame - SDL_GetTicks())) - 10;
	if ( ticks > 0 ) {
		SDL_Delay(ticks);
	}
}

static void ALSA_PlayAudio(_THIS)
{
	
	int written;

	/* Write the audio data, checking for EAGAIN on broken audio drivers */
	do {
		written = SDL_NAME(pcm_writei)(pcm_out, mixbuf, mixlen);
		if (written < 0) {
			SDL_Delay(1);	/* Let a little CPU time go by */
		}
	} while (written < 0);

	/* Set the next write frame */
	next_frame += frame_ticks;

	/* If we couldn't write, assume fatal error for now */
	if ( written < 0 ) {
		this->enabled = 0;
	}
}

static Uint8 *ALSA_GetAudioBuf(_THIS)
{
	return(mixbuf);
}

static void ALSA_CloseAudio(_THIS)
{
	if ( mixbuf != NULL ) {
		SDL_FreeAudioMem(mixbuf);
		mixbuf = NULL;
	}
	if (pcm_out)
	{
		SDL_NAME(pcm_close)(pcm_out);
		pcm_out = NULL;
	}
}

static int ALSA_OpenAudio(_THIS, SDL_AudioSpec *spec)
{
	if (mixbuf)
	{
		free(mixbuf);
		mixbuf = NULL;
	}

	/* Try for a closest match on audio format */
	switch ( spec->format )
	{
		case AUDIO_S16LSB:
			config.format = PCM_FORMAT_S16_LE;
		break;
		case AUDIO_S16MSB:
			config.format = PCM_FORMAT_S16_BE;
		break;
		default:
			printf("Couldn't find ALSA\n");
			SDL_SetError("Couldn't find any hardware audio formats");
			ALSA_CloseAudio(this);
			return(-1);
		break;
	}
	
	config.channels = spec->channels;
	config.rate = spec->freq;
    config.period_size = (spec->samples / 2);
    config.period_count = 2;
    config.start_threshold = config.period_size;
    config.silence_threshold = config.period_size * 2;
    config.stop_threshold = config.period_size * 2;
	
    pcm_out = SDL_NAME(pcm_open)(0, 0, PCM_OUT | PCM_NONBLOCK, &config);
	if ( pcm_out < 0 ) {
		SDL_SetError("Couldn't open TinyALSA card");
		return(-1);
	}
	
	/* Calculate the final parameters for this audio specification */
	SDL_CalculateAudioSpec(spec);
	frame_ticks = (float)(spec->samples*1000)/spec->freq;
	next_frame = SDL_GetTicks()+frame_ticks;

	/* Allocate mixing buffer */
	mixlen = spec->size;
	mixbuf = (Uint8 *)SDL_AllocAudioMem(mixlen);
	if ( mixbuf == NULL ) {
		ALSA_CloseAudio(this);
		return(-1);
	}
	SDL_memset(mixbuf, spec->silence, spec->size);

	/* Get the parent process id (we're the parent of the audio thread) */
	parent = getpid();

	/* We're ready to rock and roll. :-) */
	return(0);
}
