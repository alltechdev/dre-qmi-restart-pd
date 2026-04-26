CC ?= aarch64-linux-musl-gcc
CFLAGS ?= -O2 -static -Wall

all: restart-pd query-pd

restart-pd: restart-pd.c
	$(CC) $(CFLAGS) -o $@ $<

query-pd: query-pd.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f restart-pd query-pd
