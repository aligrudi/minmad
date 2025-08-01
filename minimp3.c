#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#define MINIMP3_IMPLEMENTATION
#include "minimp3.h"
#include "minmad.h"

static mp3dec_t dec;
static int mm_fd;
static char *mm_buf;
static long mm_len;
static long mm_pos;

int mm_init(char *path)
{
	struct stat st;
	int pos = 0;
	int nr;
	mp3dec_init(&dec);
	if ((mm_fd = open(path, O_RDONLY)) < 0)
		return 1;
	if (fstat(mm_fd, &st) < 0 || st.st_size == 0)
		return 1;
	mm_len = st.st_size;
	if (!(mm_buf = malloc(mm_len))) {
		close(mm_fd);
		return 1;
	}
	while ((nr = read(mm_fd, mm_buf + pos, mm_len - pos)) > 0)
		pos += nr;
	close(mm_fd);
	return 0;
}

int mm_done(void)
{
	free(mm_buf);
	return 0;
}

int mm_decode(void *dst, long *dst_len, int *bytes, int *rate, int *ch, int *bits)
{
	mp3dec_frame_info_t info = {0};
	int n = mp3dec_decode_frame(&dec, (void *) mm_buf + mm_pos, mm_len - mm_pos, (void *) dst, &info);
	*dst_len = n * info.channels * 2;
	*rate = info.hz;
	*ch = info.channels;
	*bits = 16;
	*bytes = info.frame_bytes;
	mm_pos += info.frame_bytes;
	return n <= 0 && info.frame_bytes == 0;
}

int mm_mark(long *pos, long *len)
{
	*pos = mm_pos;
	*len = mm_len;
	return 0;
}

int mm_seek(long pos)
{
	if (pos >= 0 && pos < mm_len)
		mm_pos = pos;
	return 0;
}
