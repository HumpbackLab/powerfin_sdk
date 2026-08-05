CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra

all: rk-blheli32-info rk-blheli32-bf4way

rk-blheli32-info: rk-blheli32-info.c
	$(CC) $(CFLAGS) -o $@ $<

rk-blheli32-bf4way: rk-blheli32-bf4way.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f rk-blheli32-info rk-blheli32-bf4way

.PHONY: all clean
