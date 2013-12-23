#ifndef MPMC_H_
#define MPMC_H_

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>  // need for getpid()
#include <signal.h> // needed for kill

#define MPMC_EMPTY 0
#define MPMC_READING 1
#define MPMC_WRITING 2
#define MPMC_FULL 3

// TODO:
// * memory barrier on writes

typedef struct
{
  // qsize = elsize+1, qend=(qsize)*nel
  const size_t nel, elsize, qsize, qend;
  size_t nproducers, nconsumers;
  volatile size_t noccupied, last_read, last_write;
  volatile char *const data; // [<state><element>]+

  // reads block until success if open is != 0
  // otherwise they return 0
  volatile char open;

  // Mutexes for waiting
  // number of write threads waiting for read, and vice versa
  volatile size_t sleeping_prods, sleeping_cons;
  pthread_mutex_t read_wait_mutex, write_wait_mutex;
  pthread_cond_t read_wait_cond, write_wait_cond;
} MPMCQueue;

// Allocate a new queue
static inline void mpmc_alloc(MPMCQueue *q, size_t nel, size_t elsize,
                              size_t nproducers, size_t nconsumers)
{
  char *data = calloc(nel, elsize+1);
  MPMCQueue tmpq = {.nel = nel, .elsize = elsize, .qsize = elsize+1,
                    .qend = (elsize+1)*nel, .data = data,
                    .nproducers = nproducers, .nconsumers = nconsumers,
                    .noccupied = 0, .open = 1,
                    .last_read = 0, .last_write = 0,
                    .sleeping_prods = 0, .sleeping_cons = 0};
  memcpy(q, &tmpq, sizeof(MPMCQueue));
  if(pthread_mutex_init(&q->read_wait_mutex, NULL) != 0 ||
     pthread_mutex_init(&q->write_wait_mutex, NULL) != 0)
  { fprintf(stderr, "pthread_mutex init failed\n"); abort(); }
  if(pthread_cond_init(&q->read_wait_cond, NULL) != 0 ||
    pthread_cond_init(&q->write_wait_cond, NULL) != 0)
  { fprintf(stderr, "pthread_cond init failed\n"); abort(); }
}

// Deallocate a new queue
static inline void mpmc_dealloc(MPMCQueue *q)
{
  pthread_cond_destroy(&q->read_wait_cond);
  pthread_mutex_destroy(&q->read_wait_mutex);
  free((char*)q->data);
}

// Returns number of bytes read (0 or q->nel)
static inline int mpmc_read(MPMCQueue *q, void *p)
{
  size_t i, nocc, s = q->last_read;
  while(1)
  {
    if(q->noccupied == 0) {
      if(!q->open) return 0;
      else {
        // Wait on write
        pthread_mutex_lock(&q->write_wait_mutex);
        __sync_fetch_and_add(&q->sleeping_cons, 1); // atomic q->sleeping_cons++
        while(q->noccupied == 0 && q->open)
          pthread_cond_wait(&q->write_wait_cond, &q->write_wait_mutex);
        __sync_fetch_and_sub(&q->sleeping_cons, 1); // atomic q->sleeping_cons--
        pthread_mutex_unlock(&q->write_wait_mutex);
      }
    }

    for(i = s; i < q->qend; i += q->qsize)
    {
      if(__sync_bool_compare_and_swap(&q->data[i], MPMC_FULL, MPMC_READING))
      {
        q->last_read = i;
        memcpy((char*)p, (char*)q->data+i+1, q->elsize);
        // memory barrier: must read element before setting MPMC_EMPTY
        __sync_synchronize();
        q->data[i] = MPMC_EMPTY;
        // __sync_synchronize();
        nocc = __sync_fetch_and_sub(&q->noccupied, 1); // q->noccupied--
        nocc--;
        // __sync_synchronize();

        size_t nproducers = q->nproducers-q->sleeping_prods;
        if(q->sleeping_prods && nproducers < q->nel-nocc)
        {
          // Notify when space appears in queue or queue empty
          pthread_mutex_lock(&q->read_wait_mutex);
          pthread_cond_signal(&q->read_wait_cond); // wake one
          pthread_mutex_unlock(&q->read_wait_mutex);
        }

        // printf("q->noccupied: %zu [%s] sleeping_prods: %i\n",
        //        q->noccupied, q->open ? "open" : "closed", q->sleeping_prods);

        return q->elsize;
      }
    }
    s = 0;
  }

  return 0;
}

// if til_empty, wait until queue is empty,
// otherwise wait until space
static inline void _mpmc_wait(MPMCQueue *q, char til_empty)
{
  // printf("waiting until %s\n", til_empty ? "empty" : "space");
  size_t limit = til_empty ? 0 : q->nel - 1;
  if(q->noccupied > limit) {
    pthread_mutex_lock(&q->read_wait_mutex);
    __sync_fetch_and_add(&q->sleeping_prods, 1); // atomic q->sleeping_prods++
    while(q->noccupied > limit)
      pthread_cond_wait(&q->read_wait_cond, &q->read_wait_mutex);
    __sync_fetch_and_sub(&q->sleeping_prods, 1); // atomic q->sleeping_prods--
    pthread_mutex_unlock(&q->read_wait_mutex);
  }
  // printf("done waiting %zu\n", q->noccupied);
}

static inline void mpmc_write(MPMCQueue *q, void *p)
{
  size_t i, nocc, s = q->last_write;
  while(1)
  {
    while(q->noccupied == q->nel) _mpmc_wait(q, 0);

    for(i = s; i < q->qend; i += q->qsize)
    {
      if(__sync_bool_compare_and_swap(&q->data[i], MPMC_EMPTY, MPMC_WRITING))
      {
        q->last_write = i;
        memcpy((char*)q->data+i+1, (char*)p, q->elsize);
        // memory barrier on writes: must write element before writing status
        // dev: may not be needed on x86, this is already promised
        // __sync_synchronize();
        q->data[i] = MPMC_FULL;
        // __sync_synchronize();
        nocc = __sync_fetch_and_add(&q->noccupied, 1); // q->noccupied++
        nocc++;
        // __sync_synchronize();
        // printf("WROTE [left: %zu]\n", q->noccupied);

        // if(q->noccupied == q->nel) printf("Queue full\n");

        // if occupied >= waiting threads
        // => q->noccupied >= q->nconsumers-q->sleeping_cons
        size_t nconsumers = q->nconsumers - q->sleeping_cons;
        if(q->sleeping_cons && nconsumers < nocc)
        {
          // Notify when element written
          pthread_mutex_lock(&q->write_wait_mutex);
          pthread_cond_signal(&q->write_wait_cond); // wake one
          pthread_mutex_unlock(&q->write_wait_mutex);
        }

        return;
      }
    }

    s = 0;
    // printf("Duh!\n");
  }
}

// Wait until the queue is empty, keep mpmc_read() blocking
static inline void mpmc_wait_til_empty(MPMCQueue *q)
{
  _mpmc_wait(q, 1);
}

// Close causes mpmc_read() to return 0 if queue is empty
static inline void mpmc_close(MPMCQueue *q) {
  q->open = 0;
  if(q->sleeping_cons) {
    pthread_mutex_lock(&q->write_wait_mutex);
    pthread_cond_broadcast(&q->write_wait_cond); // wake one
    pthread_mutex_unlock(&q->write_wait_mutex);
  }
}

static inline void mpmc_reopen(MPMCQueue *q) {
  q->open = 1;
}

#endif /* MPMC_H_ */
