/*
 * Copyright (C) 2022 by Don F. Becker <don@donfbecker.com>
 * Derived from rtl_sdr.c by Kyle Keen <keenerd@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <string.h>
#include <math.h>
#include <getopt.h>

/********************/
/* ALSA Sound Stuff */
/********************/
#include <alsa/asoundlib.h>

#define AUDIO_DEVICE "default"
#define AUDIO_SAMPLE_RATE 48000
#define AUDIO_CHANNELS 2
#define AUDIO_BUFFER_LENGTH 16384 // Should be multiple of 16384, so that SDR buffer is as well

float g_audio_buffer[(AUDIO_BUFFER_LENGTH * AUDIO_CHANNELS)];
snd_pcm_t *g_audio_handle;
snd_pcm_format_t format = SND_PCM_FORMAT_FLOAT;
static int audio_channel = 0;

/*****************/
/* RTL-SDR Stuff */
/*****************/
#include <rtl-sdr.h>
#include "convenience.h"

#define SDR_SAMPLE_RATE         240000
#define SDR_SAMPLE_RATIO        5
#define SDR_BUFFER_LENGTH       (AUDIO_BUFFER_LENGTH * SDR_SAMPLE_RATIO * 2)

static rtlsdr_dev_t *sdr_dev = NULL;

static int do_exit = 0;
static uint32_t bytes_to_read = 0;

void usage(void) {
	fprintf(stderr,
		"rtl-sdr-audio Usage:\n"
		"\t -f frequency [Hz]\n"
		"\t[-d device_index (default: 0)]\n"
		"\t[-g gain (default: 0 for auto)]\n"
		"\t[-p ppm_error (default: 0)]\n"
		"\t[-c audio_channel [0:both, 1:left, 2:right] (default: 0)]\n"
	);
	exit(1);
}

static void sighandler(int signum) {
	fprintf(stderr, "Signal caught, exiting!\n");
	do_exit = 1;
	rtlsdr_cancel_async(sdr_dev);
}

static void sdr_callback(unsigned char *iqBuffer, uint32_t len, void *ctx) {
	double sumI, sumQ;
	double maxA = 0;
	double i, q, a, v;

	//fprintf(stderr, "buffer is filled with %d bytes.\n", len);

	if (do_exit) return;

	maxA = 0;
	for(int j = 0; j < AUDIO_BUFFER_LENGTH; j++) {
		sumI = sumQ = 0;
		for(int k = 0; k < SDR_SAMPLE_RATIO; k++) {
			sumI += (double)(((iqBuffer[(j * SDR_SAMPLE_RATIO * 2) + (k * 2)] & 0xFF) - 127.5) / 127.5);
			sumQ += (double)(((iqBuffer[(j * SDR_SAMPLE_RATIO * 2) + (k * 2) + 1] & 0xFF) - 127.5) / 127.5);
		}

		i = (sumI / SDR_SAMPLE_RATIO);
		q = (sumQ / SDR_SAMPLE_RATIO);
		a = sqrt((i * i) + (q * q));
		if(a > maxA) maxA = a;

		if(audio_channel == 0 || audio_channel == 1) g_audio_buffer[(j*2)] = q;
		if(audio_channel == 0 || audio_channel == 2) g_audio_buffer[(j*2)+1] = q;
	}

	fprintf(stderr, "\33[2K\r%0*d\r", (int)(maxA * 30), 0);
	snd_pcm_writei(g_audio_handle, g_audio_buffer, AUDIO_BUFFER_LENGTH);
}

int main(int argc, char **argv) {
	int n_read;
	int r, opt;
        int err;

	int sdr_dev_index = 0;
	int sdr_dev_given = 0;
	int sdr_gain = 0;
	int sdr_ppm_error = 0;

	uint32_t sdr_frequency = 148039000;
	uint32_t sdr_sample_rate = SDR_SAMPLE_RATE;

	struct sigaction sigact;

	sigact.sa_handler = sighandler;
	sigemptyset(&sigact.sa_mask);
	sigact.sa_flags = 0;
	sigaction(SIGINT, &sigact, NULL);
	sigaction(SIGTERM, &sigact, NULL);
	sigaction(SIGQUIT, &sigact, NULL);
	sigaction(SIGPIPE, &sigact, NULL);

	while ((opt = getopt(argc, argv, "d:f:g:p:c:")) != -1) {
		switch (opt) {
			case 'd':
				sdr_dev_index = verbose_device_search(optarg);
				sdr_dev_given = 1;
				break;
			case 'f':
				sdr_frequency = (uint32_t)atofs(optarg);
				break;
			case 'g':
				sdr_gain = (int)(atof(optarg) * 10); /* tenths of a dB */
				break;
			case 'p':
				sdr_ppm_error = atoi(optarg);
				break;

			case 'c':
				audio_channel = atoi(optarg);
				break;

			default:
				usage();
				break;
		}
	}

        /* Prepare Audio Device */
        if ((err = snd_pcm_open(&g_audio_handle, AUDIO_DEVICE, SND_PCM_STREAM_PLAYBACK, 0)) < 0) {
            fprintf(stderr, "Error opening audio device: %s\n", snd_strerror(err));
            return EXIT_FAILURE;
        }

        if ((err = snd_pcm_set_params(g_audio_handle, format, SND_PCM_ACCESS_RW_INTERLEAVED, AUDIO_CHANNELS, AUDIO_SAMPLE_RATE, 1, 500000)) < 0) {
            fprintf(stderr, "Error setting audio device parameters: %s\n", snd_strerror(err));
            return EXIT_FAILURE;
        }

	/* Prepare RTL-SDR */
	if (!sdr_dev_given) sdr_dev_index = verbose_device_search("0");
	if (sdr_dev_index < 0) exit(1);

	r = rtlsdr_open(&sdr_dev, (uint32_t)sdr_dev_index);
	if (r < 0) {
		fprintf(stderr, "Failed to open rtlsdr device #%d.\n", sdr_dev_index);
		exit(1);
	}

	/* Set the sample rate */
	verbose_set_sample_rate(sdr_dev, sdr_sample_rate);

	/* Set the frequency */
	verbose_set_frequency(sdr_dev, sdr_frequency);

	if (sdr_gain == 0) {
		 /* Enable automatic gain */
		verbose_auto_gain(sdr_dev);
	} else {
		/* Enable manual gain */
		sdr_gain = nearest_gain(sdr_dev, sdr_gain);
		verbose_gain_set(sdr_dev, sdr_gain);
	}

	verbose_ppm_set(sdr_dev, sdr_ppm_error);

	/* Reset endpoint before we start reading from it (mandatory) */
	verbose_reset_buffer(sdr_dev);

	fprintf(stderr, "Reading samples in async mode...\n");
	r = rtlsdr_read_async(sdr_dev, sdr_callback, NULL, 0, SDR_BUFFER_LENGTH);

	if (do_exit) fprintf(stderr, "\nUser cancel, exiting...\n");
	else fprintf(stderr, "\nLibrary error %d, exiting...\n", r);

	rtlsdr_close(sdr_dev);

        snd_pcm_drain(g_audio_handle);
        snd_pcm_close(g_audio_handle);

	return r >= 0 ? r : -r;
}
