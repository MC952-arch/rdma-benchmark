CC=gcc
CFLAGS=-Wall -Werror -O2
INCLUDES=
LDFLAGS=
LIBS=-lrdmacm -libverbs

SRCS=main.c
OBJS=$(SRCS:.c=.o)
PROG=rdma-benchmark

all: $(PROG)

debug: CFLAGS=-Wall -Werror -g -DDEBUG
debug: $(PROG)

.c.o:
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

$(PROG): $(OBJS)
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $(OBJS) $(LDFLAGS) $(LIBS)

clean:
	$(RM) *.o *~ $(PROG)

run:
	@./rdma-benchmark -m 128 -n 32 -s 16 -i 1 -o 1 -b 4
