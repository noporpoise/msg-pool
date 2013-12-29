#include <stdlib.h>
#include <stdio.h>
#include <unistd.h> // getpid
#include <stdarg.h>
#include "mpmc.h"

// Queue
MPMCQueue q;
size_t qlen = 1000;

// 100 producers, 100 consumers, each producer produces 10000
size_t nproducers = 10, nconsumers = 10, proc_count = 1000000;

void print_usage()
{
  printf("usage: test [options]\n"
"    -p <p>  Number of producer threads [default: %zu]\n"
"    -c <c>  Number of consumer threads [default: %zu]\n"
"    -n <n>  Number of messages per producer [default: %zu]\n"
"    -q <q>  Queue length [default: %zu]\n",
         nproducers, nconsumers, proc_count, qlen);
  exit(EXIT_FAILURE);
}

struct TestThread {
  pthread_t th;
  size_t id, result;
};

struct TestThread *producers, *consumers;

void* produce(void *ptr)
{
  struct TestThread *prod = (struct TestThread*)ptr;
  size_t w, id = prod->id, start = id * proc_count, end = start + proc_count;
  printf("Created producer %zu\n", id);
  for(w = start; w < end; w++) mpmc_write(&q, &w);
  printf("Producer %zu finished!\n", id);
  pthread_exit(NULL);
}

void* consume(void *ptr)
{
  struct TestThread *cons = (struct TestThread*)ptr;
  size_t id = cons->id, r, sum = 0;
  printf("Created consumer %zu\n", id);
  while(mpmc_read(&q, &r)) {
    // printf("%zu\n", r);
    sum += r;
  }
  printf("Consumer %zu finished!\n", id);
  cons->result = sum;
  pthread_exit(NULL);
}

void init_threads()
{
  size_t i; int rc;
  pthread_attr_t thread_attr;
  pthread_attr_init(&thread_attr);
  pthread_attr_setdetachstate(&thread_attr, PTHREAD_CREATE_JOINABLE);

  producers = malloc(nproducers * sizeof(struct TestThread));
  consumers = malloc(nconsumers * sizeof(struct TestThread));

  // create consumers
  for(i = 0; i < nconsumers; i++) {
    consumers[i].id = i;
    rc = pthread_create(&consumers[i].th, &thread_attr, consume, &consumers[i]);
    if(rc != 0) { fprintf(stderr, "Creating thread failed\n"); exit(-1); }
  }

  // create producers
  for(i = 0; i < nproducers; i++) {
    producers[i].id = i;
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
  mpmc_wait_til_empty(&q);

  for(i = 0; i < 100; i++)
    mpmc_write(&q, &i);

  size_t extra_sum = 100*(99/2.0);

  mpmc_close(&q);
  printf("waiting for consumers to finish...\n");
  size_t sum = 0, nmsg = proc_count * nproducers;

  // Wait until finished
  for(i = 0; i < nconsumers; i++) {
    rc = pthread_join(consumers[i].th, NULL);
    if(rc != 0) { fprintf(stderr, "Join thread failed\n"); exit(-1); }
    sum += consumers[i].result;
  }

  size_t corr_sum = nmsg*((nmsg-1)/2.0) + extra_sum;
  printf("messages: %zu result: %zu [%s %zu]\n",
         nmsg, sum, sum == corr_sum ? "PASS" : "FAIL", corr_sum);

  pthread_attr_destroy(&thread_attr);
  free(producers);
  free(consumers);
}

int main(int argc, char **argv)
{
  // Read args
  int c;

  while ((c = getopt(argc, argv, "p:c:n:q:h")) >= 0)
    if (c == 'p') nproducers = atoi(optarg);
    else if (c == 'c') nconsumers = atoi(optarg);
    else if (c == 'n') proc_count = atoi(optarg);
    else if (c == 'q') qlen = atoi(optarg);
    else print_usage();

  // Create queue of ints of length qlen
  mpmc_alloc(&q, qlen, sizeof(size_t), nproducers, nconsumers);

  printf("nproducers: %zu nconsumers: %zu n: %zu qlen: %zu\n",
         nproducers, nconsumers, proc_count, qlen);

  init_threads();

  printf("done.\n");
  mpmc_dealloc(&q);

  return EXIT_SUCCESS;
}
