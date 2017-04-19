CC=gcc

IPV4_TARGETS=ping
TARGETS=$(IPV4_TARGETS)

all: $(TARGETS)

ping: ping.c
	$(CC) -o ping ping.c

clean:
	rm -r *.o $(TARGETS)
