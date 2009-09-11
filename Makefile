CC = cc
CFLAGS = -std=gnu89 -pedantic -Wall -O2
LDFLAGS = -lmad -lasound

all: minmad
.c.o:
	$(CC) -c $(CFLAGS) $<
minmad: minmad.o
	$(CC) $(LDFLAGS) -o $@ $^
clean:
	rm -f *.o minmad

