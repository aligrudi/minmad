CC = cc
CFLAGS = -Wall -O2
LDFLAGS = -lmad

OBJS = minmad.o

all: minmad
.c.o:
	$(CC) -c $(CFLAGS) $<
minmad: $(OBJS)
	$(CC) -o $@ $(OBJS) $(LDFLAGS)
clean:
	rm -f *.o minmad

