CC = cc
CFLAGS = -Wall -O2
LDFLAGS =

OBJS = minmad.o minimp3.o

all: minmad
.c.o:
	$(CC) -c $(CFLAGS) $<
minmad: $(OBJS)
	$(CC) -o $@ $(OBJS) $(LDFLAGS)
clean:
	rm -f *.o minmad

