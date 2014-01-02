
ifdef DEBUG
	CFLAGS = -g -ggdb -O0
else
	CFLAGS = -O2
endif

all: test

clean:
	rm -rf test test.dSYM ptest

test: test.c msgpool.h
	$(CC) -Wall -Wextra -std=c99 $(CFLAGS) -o test test.c -lpthread

# ptest: ptest.c pipe/pipe.o
# 	$(CC) -Wall -Wextra -std=c99 $(CFLAGS) -o ptest -I pipe/ ptest.c pipe/pipe.o

.PHONY: all clean
