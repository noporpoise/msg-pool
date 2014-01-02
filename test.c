#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <unistd.h> // getpid
#include "msgpool.h"

#define NPROC_DEFAULT 5
#define NCONS_DEFAULT 5
#define NMESG_DEFAULT 1000000
#define QLEN_DEFAULT 1000

void print_usage()
{
  printf("usage: test [options]\n"
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
  size_t w;
  printf("Created producer %zu\n", prod->id);
  for(w = prod->start; w < prod->end; w++) msgpool_write(prod->q, &w, NULL);
  printf("Producer %zu finished!\n", prod->id);
  pthread_exit(NULL);
}

void* consume(void *ptr)
{
  struct TestThread *cons = (struct TestThread*)ptr;
  size_t r, sum = 0;
  printf("Created consumer %zu\n", cons->id);
  while(msgpool_read(cons->q, &r, NULL)) {
    // printf("%zu\n", r);
    sum += r;
  }
  printf("Consumer %zu finished!\n", cons->id);
  cons->result = sum;
  pthread_exit(NULL);
}

void run_threads(MsgPool *q, size_t nmesgs)
{
  size_t i, nproducers = q->nproducers, nconsumers = q->nconsumers;
  int rc;

  printf("nproducers: %zu nconsumers: %zu n: %zu qlen: %zu\n",
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

  // Wait for those jobs to finish before submitting more
  msgpool_wait_til_empty(q);

  for(i = 0; i < 100; i++)
    msgpool_write(q, &i, NULL);

  size_t extra_sum = 100*(99/2.0);

  msgpool_close(q);
  printf("waiting for consumers to finish...\n");
  size_t sum = 0;

  // Wait until finished
  for(i = 0; i < nconsumers; i++) {
    rc = pthread_join(consumers[i].th, NULL);
    if(rc != 0) { fprintf(stderr, "Join thread failed\n"); exit(-1); }
    sum += consumers[i].result;
  }

  size_t corr_sum = nmesgs*((nmesgs-1)/2.0) + extra_sum;
  printf("messages: %zu result: %zu [%s %zu]\n",
         nmesgs, sum, sum == corr_sum ? "PASS" : "FAIL", corr_sum);

  pthread_attr_destroy(&thread_attr);
  free(producers);
  free(consumers);
}

void set_zero(char *ptr, size_t i, void *args)
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

  // Read args
  int c;

  while ((c = getopt(argc, argv, "p:c:n:q:h")) >= 0)
    if (c == 'p') nproducers = atoi(optarg);
    else if (c == 'c') nconsumers = atoi(optarg);
    else if (c == 'n') nmesgs = atoi(optarg);
    else if (c == 'q') qlen = atoi(optarg);
    else print_usage();

  // Create pool of ints of length qlen
  MsgPool q;
  msgpool_alloc(&q, qlen, sizeof(size_t), nproducers, nconsumers);

  msgpool_iterate(&q, set_zero, NULL);

  run_threads(&q, nmesgs);

  msgpool_dealloc(&q);

  printf("Done.\n");
  return EXIT_SUCCESS;
}
