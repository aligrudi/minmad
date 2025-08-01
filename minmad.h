int mm_init(char *path);
int mm_done(void);
int mm_decode(void *dst, long *dst_len, int *mp3_bytes, int *rate, int *ch, int *bits);
int mm_mark(long *pos, long *len);
int mm_seek(long pos);
