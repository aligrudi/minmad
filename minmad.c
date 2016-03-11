/*
 * minmad - a minimal mp3 player using libmad and oss
 *
 * Copyright (C) 2009-2016 Ali Gholami Rudi
 *
 * This program is released under the Modified BSD license.
 */
#include <ctype.h>
#include <fcntl.h>
#include <pty.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/poll.h>
#include <termios.h>
#include <unistd.h>
#include <sys/soundcard.h>
#include <mad.h>

#define CTRLKEY(x)	((x) - 96)
#define MIN(a, b)	((a) < (b) ? (a) : (b))
#define MAX(a, b)	((a) > (b) ? (a) : (b))

static struct mad_decoder maddec;
static int afd;			/* oss fd */

static char filename[128];
static int mfd;			/* input file descriptor */
static long msize;		/* file size */
static unsigned char mbuf[1 << 16];
static long mpos;		/* the position of mbuf[] */
static long mlen;		/* data in mbuf[] */
static long moff;		/* offset into mbuf[] */
static long mark[256];		/* mark positions */
static int frame_sz;		/* frame size */
static int frame_ms;		/* frame duration in milliseconds */
static int played;		/* playing time in milliseconds */
static int rate;		/* current oss sample rate */
static int topause;		/* planned pause (compared with played) */

static int exited;
static int paused;
static int domark;
static int dojump;
static int doseek;
static int count;

static int oss_open(void)
{
	afd = open("/dev/dsp", O_WRONLY);
	return afd < 0;
}

static void oss_close(void)
{
	if (afd > 0)		/* zero fd is used for input */
		close(afd);
	afd = 0;
	rate = 0;
}

static void oss_conf(int rate, int ch, int bits)
{
	ioctl(afd, SOUND_PCM_WRITE_CHANNELS, &ch);
	ioctl(afd, SOUND_PCM_WRITE_BITS, &bits);
	ioctl(afd, SOUND_PCM_WRITE_RATE, &rate);
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
	int per = muldiv64(mpos + moff, 1000, msize);
	int loc = muldiv64(mpos + moff, frame_ms, frame_sz * 1000);
	printf("%c %02d.%d%%  (%d:%02d:%02d - %04d.%ds)   [%s]\r",
		paused ? (afd < 0 ? '*' : ' ') : '>',
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

static void seek(long pos)
{
	mark['\''] = mpos + moff;
	mpos = MAX(0, MIN(msize, pos));
	doseek = 1;
}

static void cmdseekrel(int n)
{
	int diff = muldiv64(n, frame_sz * 1000, frame_ms ? frame_ms : 40);
	seek(mpos + moff + diff);
}

static void cmdseek100(int n)
{
	if (n <= 100)
		seek(muldiv64(msize, n, 100));
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
			domark = 0;
			mark[c] = mpos + moff;
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
			return 1;
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

static enum mad_flow madinput(void *data, struct mad_stream *stream)
{
	int nread = stream->next_frame ? stream->next_frame - mbuf : moff;
	int nleft = mlen - nread;
	int nr = 0;
	if (doseek) {
		doseek = 0;
		nleft = 0;
		nread = 0;
		lseek(mfd, mpos, 0);
	}
	memmove(mbuf, mbuf + nread, nleft);
	if (nleft < sizeof(mbuf)) {
		if ((nr = read(mfd, mbuf + nleft, sizeof(mbuf) - nleft)) <= 0) {
			exited = 1;
			return MAD_FLOW_STOP;
		}
	}
	mlen = nleft + nr;
	mad_stream_buffer(stream, mbuf, mlen);
	mpos += nread;
	moff = 0;
	return MAD_FLOW_CONTINUE;
}

static signed int madscale(mad_fixed_t sample)
{
	sample += (1l << (MAD_F_FRACBITS - 16));	/* round */
	if (sample >= MAD_F_ONE)			/* clip */
		sample = MAD_F_ONE - 1;
	if (sample < -MAD_F_ONE)
		sample = -MAD_F_ONE;
	return sample >> (MAD_F_FRACBITS + 1 - 16);	/* quantize */
}

static void madupdate(void)
{
	int sz, ms;
	if (maddec.sync) {
		moff = maddec.sync->stream.this_frame - mbuf;
		sz = maddec.sync->stream.next_frame -
			maddec.sync->stream.this_frame;
		ms = mad_timer_count(maddec.sync->frame.header.duration,
					MAD_UNITS_MILLISECONDS);
		frame_ms = frame_ms ? ((frame_ms << 5) - frame_ms + ms) >> 5 : ms;
		frame_sz = frame_sz ? ((frame_sz << 5) - frame_sz + sz) >> 5 : sz;
	}
}

static char mixed[1 << 18];
static enum mad_flow madoutput(void *data,
			 struct mad_header const *header,
			 struct mad_pcm *pcm)
{
	int c1 = 0;
	int c2 = pcm->channels > 1 ? 1 : 0;
	int i;
	played += mad_timer_count(maddec.sync->frame.header.duration,
					MAD_UNITS_MILLISECONDS);
	for (i = 0; i < pcm->length; i++) {
		mixed[i * 4 + 0] = madscale(pcm->samples[c1][i]) & 0xff;
		mixed[i * 4 + 1] = (madscale(pcm->samples[c1][i]) >> 8) & 0xff;
		mixed[i * 4 + 2] = madscale(pcm->samples[c2][i]) & 0xff;
		mixed[i * 4 + 3] = (madscale(pcm->samples[c2][i]) >> 8) & 0xff;
	}
	if (header->samplerate != rate) {
		rate = header->samplerate;
		oss_conf(rate, 2, 16);
	}
	write(afd, mixed, pcm->length * 4);
	madupdate();
	return cmdexec() ? MAD_FLOW_STOP : MAD_FLOW_CONTINUE;
}

static enum mad_flow maderror(void *data,
				struct mad_stream *stream,
				struct mad_frame *frame)
{
	return MAD_FLOW_CONTINUE;
}

static void maddecode(void)
{
	mad_decoder_init(&maddec, NULL, madinput, 0, 0, madoutput, maderror, 0);
	while (!exited) {
		if (paused) {
			cmdwait();
			cmdexec();
		} else {
			mad_decoder_run(&maddec, MAD_DECODER_MODE_SYNC);
		}
	}
	mad_decoder_finish(&maddec);
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
	struct stat stat;
	struct termios termios;
	char *path = argc >= 2 ? argv[1] : NULL;
	if (!path)
		return 1;
	if (strchr(path, '/'))
		path = strrchr(path, '/') + 1;
	snprintf(filename, 30, "%s", path);
	mfd = open(argv[1], O_RDONLY);
	if (fstat(mfd, &stat) == -1 || stat.st_size == 0)
		return 1;
	msize = stat.st_size;
	if (oss_open()) {
		fprintf(stderr, "minmad: /dev/dsp busy?\n");
		return 1;
	}
	term_init(&termios);
	maddecode();
	oss_close();
	term_done(&termios);
	close(mfd);
	printf("\n");
	return 0;
}
