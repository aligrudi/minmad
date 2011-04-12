CC = cc
CFLAGS = -Wall -Os
LDFLAGS = -lmad

all: minmad
.c.o:
	$(CC) -c $(CFLAGS) $<
minmad: minmad.o
	$(CC) $(LDFLAGS) -o $@ $^
clean:
	rm -f *.o minmad

