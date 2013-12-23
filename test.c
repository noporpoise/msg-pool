#include <stdlib.h>
#include <stdio.h>
#include <sys/time.h> // for seeding random
#include <unistd.h> // getpid
#include "mpmc.h"

// Queue
MPMCQueue q;
const int qlen = 10000;

// 100 producers, 100 consumers, each producer produces 10000
const int nproducers = 10, nconsumers = 3, proc_count = 100000;

pthread_t prod_threads[nproducers], cons_threads[nconsumers];
int prod_ids[nproducers], cons_ids[nproducers];

void seed_random()
{
  struct timeval time;
  gettimeofday(&time, NULL);
  srand((((time.tv_sec ^ getpid()) * 1000001) + time.tv_usec));
}

void* producer(void *ptr)
{
  int w, id = *(int*)ptr, start = id * proc_count, end = start + proc_count;
  printf("Created producer %i\n", id);
  for(w = start; w < end; w++) {
    mpmc_write(&q, &w);
    // printf("prod %i: %i\n", id, w);
    // sleep(1);
  }
  printf("Producer %i finished!\n", id);
  pthread_exit(NULL);
}

void* consumer(void *ptr)
{
  int id = *(int*)ptr, r;
  printf("Created consumer %i\n", id);
  while(mpmc_read(&q, &r)) {
    // printf("cons%i: %i\n", id, r);
    printf("%i\n", r);
    // sleep(1);
  }
  printf("Consumer %i finished!\n", id);
  pthread_exit(NULL);
}

int main()
{
  int i, rc;

  // Create queue of ints of length qlen 
  mpmc_alloc(&q, qlen, sizeof(int), nproducers, nconsumers);

  pthread_attr_t thread_attr;
  pthread_attr_init(&thread_attr);
  pthread_attr_setdetachstate(&thread_attr, PTHREAD_CREATE_JOINABLE);

  // create consumers
  for(i = 0; i < nconsumers; i++) {
    cons_ids[i] = i;
    rc = pthread_create(&cons_threads[i], &thread_attr, consumer, &cons_ids[i]);
    if(rc != 0) { fprintf(stderr, "Creating thread failed\n"); exit(-1); }
  }

  // create producers
  for(i = 0; i < nproducers; i++) {
    prod_ids[i] = i;
    rc = pthread_create(&prod_threads[i], &thread_attr, producer, &prod_ids[i]);
    if(rc != 0) { fprintf(stderr, "Creating thread failed\n"); exit(-1); }
  }

  printf("waiting for producers to finish...\n");

  // Wait for producers to finish
  for(i = 0; i < nproducers; i++) {
    rc = pthread_join(prod_threads[i], NULL);
    if(rc != 0) { fprintf(stderr, "Join thread failed\n"); exit(-1); }
  }

  // Wait for those jobs to finish before submitting more
  mpmc_wait_til_empty(&q);

  for(i = 0; i < 100; i++)
    mpmc_write(&q, &i);

  mpmc_close(&q);
  printf("waiting for consumers to finish...\n");

  // Wait until finished
  for(i = 0; i < nconsumers; i++) {
    rc = pthread_join(cons_threads[i], NULL);
    if(rc != 0) { fprintf(stderr, "Join thread failed\n"); exit(-1); }
  }

  printf("done.\n");
  mpmc_dealloc(&q);
  pthread_attr_destroy(&thread_attr);

  return EXIT_SUCCESS;
}
