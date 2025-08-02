#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#define MINIMP3_IMPLEMENTATION
#include "minimp3.h"
#include "minmad.h"

#define MMBUFLEN	(1 << 20)	/* buffer length */
#define MMBUFMIN	(1 << 14)	/* minimum amount of buffer for decoding */
#define MMBUFFULL	(1 << 24)	/* load files smaller than this size */

#define MIN(a, b)	((a) < (b) ? (a) : (b))

static mp3dec_t dec;
static int mm_fd;	/* mp3 file fd */
static long mm_len;	/* mm_fd size */
static long mm_pos;	/* decoding position */

static char *buf;
static long buf_sz;
static long buf_off;
static long buf_len;

int mm_init(char *path)
{
	struct stat st;
	mp3dec_init(&dec);
	if ((mm_fd = open(path, O_RDONLY)) < 0)
		return 1;
	if (fstat(mm_fd, &st) < 0 || st.st_size == 0) {
		close(mm_fd);
		return 1;
	}
	mm_len = st.st_size;
	buf_sz = mm_len <= MMBUFFULL ? mm_len : MMBUFLEN;
	if (!(buf = malloc(buf_sz))) {
		close(mm_fd);
		return 1;
	}
	return 0;
}

int mm_done(void)
{
	free(buf);
	close(mm_fd);
	return 0;
}

int mm_decode(void *dst, long *dst_len, int *bytes, int *rate, int *ch, int *bits)
{
	mp3dec_frame_info_t info = {0};
	int n;
	/* fill buf if necessary */
	if (mm_pos < buf_off || MIN(mm_pos + MMBUFMIN, mm_len) > buf_off + buf_len) {
		int nr;
		if (mm_pos > buf_off && mm_pos < buf_off + buf_len) {
			memmove(buf, buf + (mm_pos - buf_off), buf_off + buf_len - mm_pos);
			buf_len = (buf_off + buf_len) - mm_pos;
			buf_off = mm_pos - buf_len;
		} else {
			buf_off = mm_pos;
			buf_len = 0;
		}
		lseek(mm_fd, buf_off + buf_len, 0);
		while ((nr = read(mm_fd, buf + buf_len, buf_sz - buf_len)) > 0)
			buf_len += nr;
	}
	n = mp3dec_decode_frame(&dec, (void *) buf + (mm_pos - buf_off),
		(buf_off + buf_len) - mm_pos, (void *) dst, &info);
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
