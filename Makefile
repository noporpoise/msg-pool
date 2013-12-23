
ifdef DEBUG
	CFLAGS = -g -ggdb -O0
else
	CFLAGS = -O2
endif

test: test.c mpmc.h
	$(CC) -Wall -Wextra $(CFLAGS) -o test test.c -lpthread

clean:
	rm -rf test test.dSYM

all: test

.PHONY: all clean
