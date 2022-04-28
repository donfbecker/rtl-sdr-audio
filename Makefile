CC=gcc
CFLAGS=-lm -lasound -lrtlsdr

all:
	$(CC) rtl-sdr-audio.c convenience.c -o rtl-sdr-audio $(CFLAGS)
