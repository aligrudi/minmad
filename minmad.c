#include <ctype.h>
#include <fcntl.h>
#include <pty.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <termios.h>
#include <unistd.h>
#include <alsa/asoundlib.h>
#include <mad.h>

#define CTRLKEY(x)		((x) - 96)
#define MIN(a, b)	((a) < (b) ? (a) : (b))
#define MAX(a, b)	((a) > (b) ? (a) : (b))
#define LONGJUMP	1000
#define MINIJUMP	100

#define CMD_PLAY	0
#define CMD_PAUSE	1
#define CMD_QUIT	2

static snd_pcm_t *alsa;
static unsigned char *mem;
static unsigned long len;
static struct mad_decoder decoder;
static int pos;
static int count;
static mad_timer_t played;

static struct termios termios;
static int cmd;

static int readkey(void)
{
	char b;
	if (read(STDIN_FILENO, &b, 1) <= 0)
		return -1;
	return b;
}

static int getpos(void)
{
	return decoder.sync->stream.this_frame - mem;
}

static void printinfo(void)
{
	int loc = getpos() * 1000.0 / len;
	int decis = mad_timer_count(played, MAD_UNITS_DECISECONDS);
	printf("minmad:   %3d.%d%%   %8d.%ds\r",
		loc / 10, loc % 10, decis / 10, decis % 10);
	fflush(stdout);
}

static int getcount(int def)
{
	int result = count ? count : def;
	count = 0;
	return result;
}

static int framesize(void)
{
	return decoder.sync->stream.next_frame -
		decoder.sync->stream.this_frame;
}

static void seek(int n)
{
	pos = MAX(0, MIN(len, getpos() + n * framesize()));
}

static void seek_thousands(int n)
{
	pos = len * (float) n / 1000;
	pos -= pos % framesize();
}

static int execkey(void)
{
	int c;
	while ((c = readkey()) != -1) {
		switch (c) {
		case 'j':
			seek(LONGJUMP * getcount(1));
			return 1;
		case 'k':
			seek(-LONGJUMP * getcount(1));
			return 1;
		case 'l':
			seek(MINIJUMP * getcount(1));
			return 1;
		case 'h':
			seek(-MINIJUMP * getcount(1));
			return 1;
		case '%':
			seek_thousands(getcount(0) * 10);
			return 1;
		case 'i':
			printinfo();
			break;
		case 'p':
		case ' ':
			if (cmd != CMD_PAUSE) {
				cmd = CMD_PAUSE;
				pos = getpos();
				return 1;
			}
			break;
		case 'q':
			cmd = CMD_QUIT;
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

static enum mad_flow input(void *data, struct mad_stream *stream)
{
	if (pos == len)
		return MAD_FLOW_STOP;
	mad_stream_buffer(stream, mem + pos, len - pos);
	pos = len;
	return MAD_FLOW_CONTINUE;
}

static signed int scale(mad_fixed_t sample)
{
	/* round */
	sample += (1L << (MAD_F_FRACBITS - 16));
	/* clip */
	if (sample >= MAD_F_ONE)
		sample = MAD_F_ONE - 1;
	else if (sample < -MAD_F_ONE)
		sample = -MAD_F_ONE;
	/* quantize */
	return sample >> (MAD_F_FRACBITS + 1 - 16);
}

static void push_sample(char *buf, mad_fixed_t sample)
{
	*buf++ = (sample >> 0) & 0xff;
	*buf++ = (sample >> 8) & 0xff;
}

static char mixed[1 << 20];
static enum mad_flow output(void *data,
			 struct mad_header const *header,
			 struct mad_pcm *pcm)
{
	int i;
	int right = pcm->channels > 1 ? 1 : 0;
	mad_timer_add(&played, decoder.sync->frame.header.duration);
	for (i = 0; i < pcm->length; i++) {
		push_sample(mixed + i * 4, scale(pcm->samples[0][i]));
		push_sample(mixed + i * 4 + 2, scale(pcm->samples[right][i]));
	}
	/* XXX: call snd_pcm_writei less often */
	if (snd_pcm_writei(alsa, mixed, pcm->length) < 0)
		snd_pcm_prepare(alsa);
	if (execkey())
		return MAD_FLOW_STOP;
	return MAD_FLOW_CONTINUE;
}

static enum mad_flow error(void *data,
				struct mad_stream *stream,
				struct mad_frame *frame)
{
	return MAD_FLOW_CONTINUE;
}

static void waitkey(void)
{
	struct pollfd ufds[1];
	ufds[0].fd = STDIN_FILENO;
	ufds[0].events = POLLIN;
	poll(ufds, 1, -1);
}

static void decode(void)
{
	mad_decoder_init(&decoder, NULL, input, 0, 0, output, error, 0);
	while (cmd != CMD_QUIT && pos != len) {
		if (cmd == CMD_PAUSE)
			while (readkey() != 'p')
				waitkey();
		cmd = CMD_PLAY;
		mad_decoder_run(&decoder, MAD_DECODER_MODE_SYNC);
	}
	mad_decoder_finish(&decoder);
}

static void alsa_init(void)
{
	int format = SND_PCM_FORMAT_S16_LE;
	if (snd_pcm_open(&alsa, "default", SND_PCM_STREAM_PLAYBACK, 0) < 0)
		return;
	snd_pcm_set_params(alsa, format, SND_PCM_ACCESS_RW_INTERLEAVED,
			2, 44100, 1, 500000);
	snd_pcm_prepare(alsa);
}

static void alsa_close(void)
{
	snd_pcm_close(alsa);
}

static void term_setup(void)
{
	struct termios newtermios;
	tcgetattr(STDIN_FILENO, &termios);
	newtermios = termios;
	cfmakeraw(&newtermios);
	tcsetattr(STDIN_FILENO, TCSAFLUSH, &newtermios);
	fcntl(STDIN_FILENO, F_SETFL, fcntl(STDIN_FILENO, F_GETFL) | O_NONBLOCK);
}

static void term_cleanup(void)
{
	tcsetattr(STDIN_FILENO, 0, &termios);
}

int main(int argc, char *argv[])
{
	struct stat stat;
	int fd;
	if (argc < 2)
		return 1;
	fd = open(argv[1], O_RDONLY);
	if (fstat(fd, &stat) == -1 || stat.st_size == 0)
		return 1;
	mem = mmap(0, stat.st_size, PROT_READ, MAP_SHARED, fd, 0);
	len = stat.st_size;
	if (mem == MAP_FAILED)
		return 1;
	term_setup();
	alsa_init();
	decode();
	alsa_close();
	term_cleanup();
	munmap(mem, stat.st_size);
	close(fd);
	return 0;
}
