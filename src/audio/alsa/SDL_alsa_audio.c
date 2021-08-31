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

/* The tag name used by DSP audio */
#define ALSA_DRIVER_NAME         "alsa"

/* Audio driver functions */
static int ALSA_OpenAudio(_THIS, SDL_AudioSpec *spec);
static void ALSA_WaitAudio(_THIS);
static void ALSA_PlayAudio(_THIS);
static Uint8 *ALSA_GetAudioBuf(_THIS);
static void ALSA_CloseAudio(_THIS);

/* The audio device handle */
static struct pcm *pcm_out;
static struct pcm_config config;

/* Audio driver bootstrap functions */

static int Audio_Available(void)
{
	/*int fd;
	int available;

	available = 0;
	fd = SDL_OpenAudioPath(NULL, 0, OPEN_FLAGS, 0);
	if ( fd >= 0 ) {
		available = 1;
		close(fd);
	}*/
	return(1);
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
}

static void ALSA_PlayAudio(_THIS)
{
    pcm_writei(pcm_out, mixbuf, mixlen);

#ifdef DEBUG_AUDIO
	fprintf(stderr, "Wrote %d bytes of audio data\n", mixlen);
#endif
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
		pcm_close(pcm_out);
		pcm_out = NULL;
	}
}

static int ALSA_OpenAudio(_THIS, SDL_AudioSpec *spec)
{
	/* Calculate the final parameters for this audio specification */
	SDL_CalculateAudioSpec(spec);
	
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
    config.period_size = spec->samples;
    config.period_count = 2;
    config.start_threshold = config.period_size;
    config.silence_threshold = config.period_size * 2;
    config.stop_threshold = config.period_size * 2;
	
    pcm_out = pcm_open(0, 0, PCM_OUT | PCM_NONBLOCK, &config);

	/* Allocate mixing buffer */
	mixlen = spec->size;
	mixbuf = (Uint8 *)SDL_AllocAudioMem(mixlen);
	if ( mixbuf == NULL ) {
		ALSA_CloseAudio(this);
		return(-1);
	}
	SDL_memset(mixbuf, spec->silence, spec->size);

	/* We're ready to rock and roll. :-) */
	return(0);
}
