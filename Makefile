CC = cc
CFLAGS = -Wall -O2
LDFLAGS = -lmad

all: minmad
.c.o:
	$(CC) -c $(CFLAGS) $<
minmad: minmad.o
	$(CC) $(LDFLAGS) -o $@ $^
clean:
	rm -f *.o minmad

