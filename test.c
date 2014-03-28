// request decent POSIX version
#define _XOPEN_SOURCE 700
#define _BSD_SOURCE

#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <unistd.h> // getpid
#include "msgpool.h"

#define NPROC_DEFAULT 5
#define NCONS_DEFAULT 5
#define NMESG_DEFAULT 1000000
#define QLEN_DEFAULT 1000

void print_usage() __attribute__((noreturn));

void print_usage()
{
  printf("usage: test [options]\n"
"    -s      Use spinlock\n"
"    -y      Use yield\n"
"    -m      Use mutexes\n"
"    -p <p>  Number of producer threads [default: %i]\n"
"    -c <c>  Number of consumer threads [default: %i]\n"
"    -n <n>  Number of messages per producer [default: %i]\n"
"    -q <q>  Pool capacity [default: %i]\n",
         NPROC_DEFAULT, NCONS_DEFAULT, NMESG_DEFAULT, QLEN_DEFAULT);
  exit(EXIT_FAILURE);
}

struct TestThread {
  MsgPool *q;
  pthread_t th;
  size_t id, start, end;
  size_t result;
};

struct TestThread *producers, *consumers;

void* produce(void *ptr)
{
  const struct TestThread *prod = (const struct TestThread*)ptr;
  MsgPool *pool = prod->q;
  assert(pool->elsize == sizeof(size_t));
  size_t w;
  printf("Created producer %zu\n", prod->id);
  for(w = prod->start; w < prod->end; w++) msgpool_write(pool, &w, NULL);
  printf("Producer %zu finished!\n", prod->id);
  pthread_exit(NULL);
}

void* consume(void *ptr)
{
  struct TestThread *cons = (struct TestThread*)ptr;
  MsgPool *pool = cons->q;
  assert(pool->elsize == sizeof(size_t));
  size_t r, sum = 0; int pos;
  printf("Created consumer %zu\n", cons->id);
  while((pos = msgpool_claim_read(pool)) != -1) {
    memcpy(&r, msgpool_get_ptr(pool, pos), sizeof(size_t));
    msgpool_release(pool, pos, MPOOL_EMPTY);
    // printf("%zu\n", r);
    sum += r;
  }
  printf("Consumer %zu finished!\n", cons->id);
  cons->result = sum;
  pthread_exit(NULL);
}

void run_threads(MsgPool *q, size_t nmesgs, size_t nproducers, size_t nconsumers)
{
  size_t i;
  int rc;

  const char *lockstr[] = {"spinlocks [-s]", "yield [-y]", "mutexes [-m]"};

  printf("Using %s\n", lockstr[(int)q->locking]);

  printf("nproducers [-p]: %zu nconsumers [-c]: %zu "
         "messages [-n]: %zu qlen [-q]: %zu\n",
         nproducers, nconsumers, nmesgs, q->nel);

  // Thread attributes
  pthread_attr_t thread_attr;
  pthread_attr_init(&thread_attr);
  pthread_attr_setdetachstate(&thread_attr, PTHREAD_CREATE_JOINABLE);

  producers = malloc(nproducers * sizeof(struct TestThread));
  consumers = malloc(nconsumers * sizeof(struct TestThread));

  // create consumers
  for(i = 0; i < nconsumers; i++) {
    consumers[i].id = i;
    consumers[i].q = q;
    rc = pthread_create(&consumers[i].th, &thread_attr, consume, &consumers[i]);
    if(rc != 0) { fprintf(stderr, "Creating thread failed\n"); exit(-1); }
  }

  size_t start, end = 0, msg_per_prod = nmesgs/nproducers;

  // create producers
  for(i = 0; i < nproducers; i++) {
    start = end;
    end = (i+1 == nproducers ? nmesgs : end + msg_per_prod);
    producers[i].id = i;
    producers[i].q = q;
    producers[i].start = start;
    producers[i].end = end;
    rc = pthread_create(&producers[i].th, &thread_attr, produce, &producers[i]);
    if(rc != 0) { fprintf(stderr, "Creating thread failed\n"); exit(-1); }
  }

  printf("waiting for producers to finish...\n");

  // Wait for producers to finish
  for(i = 0; i < nproducers; i++) {
    rc = pthread_join(producers[i].th, NULL);
    if(rc != 0) { fprintf(stderr, "Join thread failed\n"); exit(-1); }
  }

  // Wait until empty
  msgpool_wait_til_empty(q);

  for(i = 0; i < 100; i++)
    msgpool_write(q, &i, NULL);

  size_t extra_sum = (size_t)(100*(99/2.0));

  sleep(1);

  printf("waiting for consumers to finish...\n");
  msgpool_close(q);

  size_t sum = 0;

  // Wait until finished
  for(i = 0; i < nconsumers; i++) {
    rc = pthread_join(consumers[i].th, NULL);
    if(rc != 0) { fprintf(stderr, "Join thread failed\n"); exit(-1); }
    sum += consumers[i].result;
  }

  size_t corr_sum = (size_t)(nmesgs*((nmesgs-1)/2.0) + extra_sum);
  printf("messages: %zu result: %zu [%s %zu]\n",
         nmesgs, sum, sum == corr_sum ? "PASS" : "FAIL", corr_sum);

  pthread_attr_destroy(&thread_attr);
  free(producers);
  free(consumers);
}

void set_zero(void *ptr, size_t i, void *args)
{
  (void)args;
  memcpy(ptr, &i, sizeof(size_t));
}

int main(int argc, char **argv)
{
  // Defaults
  // pool size = 100
  size_t qlen = QLEN_DEFAULT;
  // 100 producers, 100 consumers
  size_t nproducers = NPROC_DEFAULT, nconsumers = NCONS_DEFAULT;
  // pass 1000000 messages
  size_t nmesgs = NMESG_DEFAULT;

  // use spinlock instead of mutex
  int use_spinlock = 0, use_mutexes = 0, use_yield = 0;

  // Read args
  int c;

  while ((c = getopt(argc, argv, "p:c:n:q:smy")) >= 0)
    if (c == 'p') nproducers = (size_t)atoi(optarg);
    else if (c == 'c') nconsumers = (size_t)atoi(optarg);
    else if (c == 'n') nmesgs = (size_t)atoi(optarg);
    else if (c == 'q') qlen = (size_t)atoi(optarg);
    else if (c == 's') use_spinlock = 1;
    else if (c == 'm') use_mutexes = 1;
    else if (c == 'y') use_yield = 1;
    else print_usage();

  if(optind < argc) print_usage();
  if(use_spinlock + use_mutexes + use_yield > 1) print_usage();
  if(use_spinlock + use_mutexes + use_yield == 0) use_spinlock = 1;

  // Create pool of ints of length qlen
  MsgPool q;
  if(use_spinlock)
    msgpool_alloc_spinlock(&q, qlen, sizeof(size_t));
  else if(use_mutexes)
    msgpool_alloc_mutex(&q, qlen, sizeof(size_t));
  else
    msgpool_alloc_yield(&q, qlen, sizeof(size_t));

  msgpool_iterate(&q, set_zero, NULL);

  run_threads(&q, nmesgs, nproducers, nconsumers);

  msgpool_dealloc(&q);

  printf("Done.\n");
  return EXIT_SUCCESS;
}
