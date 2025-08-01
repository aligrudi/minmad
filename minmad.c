/*
 * MINMAD - A MINIMAL MP3 PLAYER USING MINIMP3 AND OSS
 *
 * Copyright (C) 2009-2024 Ali Gholami Rudi
 *
 * This program is released under the Modified BSD license.
 */
#include <ctype.h>
#include <fcntl.h>
#include <poll.h>
#include <pty.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>
#include <sys/soundcard.h>
#include "minmad.h"

#define MIN(a, b)	((a) < (b) ? (a) : (b))
#define MAX(a, b)	((a) > (b) ? (a) : (b))

static int oss_fd;		/* oss fd */

static char filename[128];
static char *ossdsp;		/* oss device */
static long mark[256];		/* mark positions */
static int frame_sz;		/* last frame size */
static int frame_ms;		/* last frame duration in milliseconds */
static int played;		/* playing time in milliseconds */
static int oss_rate, oss_ch, oss_bits;
static int topause;		/* planned pause (compared with played) */

static int exited;
static int paused;
static int domark;
static int dojump;
static int doseek;
static int count;

static int oss_open(void)
{
	oss_fd = open(ossdsp, O_WRONLY);
	return oss_fd < 0;
}

static void oss_close(void)
{
	if (oss_fd > 0)		/* zero fd is used for input */
		close(oss_fd);
	oss_fd = 0;
	oss_rate = 0;
}

static void oss_conf(int rate, int ch, int bits)
{
	int frag = 0x0003000b;	/* 0xmmmmssss: 2^m fragments of size 2^s each */
	ioctl(oss_fd, SOUND_PCM_WRITE_CHANNELS, &ch);
	ioctl(oss_fd, SOUND_PCM_WRITE_BITS, &bits);
	ioctl(oss_fd, SOUND_PCM_WRITE_RATE, &rate);
	ioctl(oss_fd, SOUND_PCM_SETFRAGMENT, &frag);
}

static int cmdread(void)
{
	char b;
	if (read(0, &b, 1) <= 0)
		return -1;
	return (unsigned char) b;
}

static void cmdwait(void)
{
	struct pollfd ufds[1];
	ufds[0].fd = 0;
	ufds[0].events = POLLIN;
	poll(ufds, 1, -1);
}

static long muldiv64(long num, long mul, long div)
{
	return (long long) num * mul / div;
}

static void cmdinfo(void)
{
	int per = 0, loc = 0;
	long pos, len;
	mm_mark(&pos, &len);
	per = muldiv64(pos, 1000, len);
	loc = muldiv64(pos, frame_ms, (frame_sz ? frame_sz : 1) * 1000);
	printf("%c %02d.%d%%  (%d:%02d:%02d - %04d.%ds)   [%s]\r",
		paused ? (oss_fd < 0 ? '*' : ' ') : '>',
		per / 10, per % 10,
		loc / 3600, (loc % 3600) / 60, loc % 60,
		played / 1000, (played / 100) % 10,
		filename);
	fflush(stdout);
}

static int cmdcount(int def)
{
	int result = count ? count : def;
	count = 0;
	return result;
}

static void seek(long newpos)
{
	long pos, len;
	mm_mark(&pos, &len);
	mark['\''] = pos;
	mm_seek(newpos);
	doseek = 1;
}

static void cmdseekrel(int n)
{
	int diff = muldiv64(n, frame_sz * 1000, frame_ms ? frame_ms : 40);
	long pos, len;
	mm_mark(&pos, &len);
	seek(pos + diff);
}

static void cmdseek100(int n)
{
	long pos, len;
	mm_mark(&pos, &len);
	if (n <= 100)
		seek(muldiv64(len, n, 100));
}

static void cmdseek(int n)
{
	long pos = muldiv64(n * 60, frame_sz * 1000, frame_ms ? frame_ms : 40);
	seek(pos);
}

static int cmdpause(int pause)
{
	if (!pause && paused) {
		if (oss_open())
			return 1;
		paused = 0;
	}
	if (pause && !paused) {
		oss_close();
		paused = 1;
	}
	return 0;
}

static int cmdexec(void)
{
	int c;
	if (topause > 0 && topause <= played) {
		topause = 0;
		return !cmdpause(1);
	}
	while ((c = cmdread()) >= 0) {
		if (domark) {
			long pos, len;
			mm_mark(&pos, &len);
			domark = 0;
			mark[c] = pos;
			return 0;
		}
		if (dojump) {
			dojump = 0;
			if (mark[c] > 0)
				seek(mark[c]);
			return mark[c] > 0;
		}
		switch (c) {
		case 'J':
			cmdseekrel(+600 * cmdcount(1));
			return 1;
		case 'K':
			cmdseekrel(-600 * cmdcount(1));
			return 1;
		case 'j':
			cmdseekrel(+60 * cmdcount(1));
			return 1;
		case 'k':
			cmdseekrel(-60 * cmdcount(1));
			return 1;
		case 'l':
			cmdseekrel(+10 * cmdcount(1));
			return 1;
		case 'h':
			cmdseekrel(-10 * cmdcount(1));
			return 1;
		case '%':
			cmdseek100(cmdcount(0));
			return 1;
		case 'G':
			cmdseek(cmdcount(0));
			return 1;
		case 'i':
			cmdinfo();
			break;
		case 'm':
			domark = 1;
			break;
		case '\'':
			dojump = 1;
			break;
		case 'p':
		case ' ':
			if (cmdpause(!paused))
				break;
			return 1;
		case 'P':
			topause = count ? played + cmdcount(0) * 60000 : 0;
			break;
		case 'q':
			exited = 1;
			return -1;
		case 27:
			count = 0;
			break;
		default:
			if (isdigit(c))
				count = count * 10 + c - '0';
		}
	}
	return 0;
}

static void mainloop(void)
{
	char dec[1 << 15];
	while (cmdexec() >= 0 && !exited) {
		if (paused) {
			cmdwait();
		} else {
			int rate, ch, bits;
			long len = sizeof(dec);
			if (mm_decode(dec, &len, &frame_sz, &rate, &ch, &bits))
				break;
			frame_ms = (len * 1000 / 2 / ch) / rate;
			played += frame_ms;
			if (rate != oss_rate || ch != oss_ch || bits != oss_bits) {
				oss_rate = rate;
				oss_ch = ch;
				oss_bits = bits;
				oss_conf(oss_rate, oss_ch, oss_bits);
			}
			write(oss_fd, dec, len);
		}
	}
}

static void term_init(struct termios *termios)
{
	struct termios newtermios;
	tcgetattr(0, termios);
	newtermios = *termios;
	newtermios.c_lflag &= ~ICANON;
	newtermios.c_lflag &= ~ECHO;
	tcsetattr(0, TCSAFLUSH, &newtermios);
	fcntl(0, F_SETFL, fcntl(0, F_GETFL) | O_NONBLOCK);
}

static void term_done(struct termios *termios)
{
	tcsetattr(0, 0, termios);
}

int main(int argc, char *argv[])
{
	struct termios termios;
	char *path = argc >= 2 ? argv[1] : NULL;
	if (!path)
		return 1;
	if (strchr(path, '/'))
		path = strrchr(path, '/') + 1;
	snprintf(filename, 30, "%s", path);
	mm_init(argv[1]);
	ossdsp = getenv("OSSDSP") ? getenv("OSSDSP") : "/dev/dsp";
	if (oss_open()) {
		fprintf(stderr, "minmad: /dev/dsp busy?\n");
		return 1;
	}
	term_init(&termios);
	mainloop();
	oss_close();
	term_done(&termios);
	printf("\n");
	return 0;
}
