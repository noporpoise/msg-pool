
ifdef DEBUG
	CFLAGS = -g -ggdb -O0
else
	CFLAGS = -O3
endif

all: test_msgpool

clean:
	rm -rf test_msgpool

test_msgpool: test.c msgpool.h
	$(CC) -Wall -Wextra -pedantic -std=c99 $(CFLAGS) -o $@ $< -lpthread

test: test_msgpool
	./test_msgpool

# ptest: ptest.c pipe/pipe.o
# 	$(CC) -Wall -Wextra -std=c99 $(CFLAGS) -o ptest -I pipe/ ptest.c pipe/pipe.o

.PHONY: all clean test
