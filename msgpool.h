#ifndef MSG_POOL_H_
#define MSG_POOL_H_

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>  // need for getpid()
#include <signal.h> // needed for kill
#include <assert.h>

#define MPOOL_EMPTY 0
#define MPOOL_READING 1
#define MPOOL_WRITING 2
#define MPOOL_FULL 3

// TODO:
// * check memory barrier on writes

typedef struct
{
  // qsize = elsize+1, qend=(qsize)*nel
  const size_t nel, elsize, qsize, qend;
  const size_t nproducers, nconsumers;
  volatile size_t noccupied, last_read, last_write;
  volatile char *const data; // [<state><element>]+

  // reads block until success if open is != 0
  // otherwise they return 0
  volatile char open;

  // Mutexes for waiting
  const int use_spinlock; // whether to spin or use mutex
  // number of write threads waiting for read, and vice versa
  volatile size_t sleeping_prods, sleeping_cons;
  pthread_mutex_t read_wait_mutex, write_wait_mutex;
  pthread_cond_t read_wait_cond, write_wait_cond;
} MsgPool;


static inline void msgpool_alloc_spinlock(MsgPool *q, size_t nel, size_t elsize)
{
  // 1 byte per element for locking
  char *data = calloc(nel, elsize+1);
  MsgPool tmpq = {.nel = nel, .elsize = elsize, .qsize = elsize+1,
                  .qend = (elsize+1)*nel, .data = data,
                  .nproducers = 0, .nconsumers = 0,
                  .noccupied = 0, .open = 1,
                  .last_read = 0, .last_write = 0,
                  .sleeping_prods = 0, .sleeping_cons = 0,
                  .use_spinlock = 1};

  memcpy(q, &tmpq, sizeof(MsgPool));
}

static inline void msgpool_alloc_mutex(MsgPool *q, size_t nel, size_t elsize,
                                       size_t nproducers, size_t nconsumers)
{
  assert(nconsumers > 0 && nconsumers < nel);
  assert(nproducers > 0 && nproducers < nel);

  // 1 byte per element for locking
  char *data = calloc(nel, elsize+1);
  MsgPool tmpq = {.nel = nel, .elsize = elsize, .qsize = elsize+1,
                  .qend = (elsize+1)*nel, .data = data,
                  .nproducers = nproducers, .nconsumers = nconsumers,
                  .noccupied = 0, .open = 1,
                  .last_read = 0, .last_write = 0,
                  .sleeping_prods = 0, .sleeping_cons = 0,
                  .use_spinlock = 0};

  memcpy(q, &tmpq, sizeof(MsgPool));

  if(pthread_mutex_init(&q->read_wait_mutex, NULL) != 0 ||
     pthread_mutex_init(&q->write_wait_mutex, NULL) != 0)
  {
    fprintf(stderr, "pthread_mutex init failed\n");
    abort();
  }
  if(pthread_cond_init(&q->read_wait_cond, NULL) != 0 ||
    pthread_cond_init(&q->write_wait_cond, NULL) != 0)
  {
    fprintf(stderr, "pthread_cond init failed\n");
    abort();
  }
}

// Deallocate a new pool
static inline void msgpool_dealloc(MsgPool *q)
{
  pthread_cond_destroy(&q->read_wait_cond);
  pthread_mutex_destroy(&q->read_wait_mutex);
  free((char*)q->data);
}

// Iterate over elements in the pool
// calls func(el,idx,args) with idx being 0,1,2... and el a pointer to the
// element (beware: not aligned in memory)
// Can be used to initialise elements at the begining or clean up afterwards
static inline void msgpool_iterate(MsgPool *q,
                                   void (*func)(char *el, size_t idx, void *args),
                                   void *args)
{
  size_t i; char *ptr, *data = (char*)q->data;
  for(i = 0, ptr = data+1; i < q->nel; i++, ptr += q->qsize) {
    func(ptr, i, args);
  }
}

// Returns number of bytes read (0 or q->nel)
static inline int msgpool_read(MsgPool *q, void *restrict p,
                               const void *restrict swap)
{
  size_t i, nocc, s = q->last_read;
  while(1)
  {
    if(q->noccupied == 0)
    {
      // Wait until something is read in or message pool is closed
      if(!q->open) return 0;
      else if(q->use_spinlock) {
        while(q->noccupied == 0 && q->open) {}
      }
      else {
        // Wait on write
        // don't need to use __sync_fetch_and_add because we have write_wait_mutex
        pthread_mutex_lock(&q->write_wait_mutex);
        // __sync_fetch_and_add(&q->sleeping_cons, 1);
        q->sleeping_cons++;

        while(q->noccupied == 0 && q->open)
          pthread_cond_wait(&q->write_wait_cond, &q->write_wait_mutex);

        // __sync_fetch_and_sub(&q->sleeping_cons, 1);
        q->sleeping_cons--;
        pthread_mutex_unlock(&q->write_wait_mutex);
      }

      if(q->noccupied == 0 && !q->open) return 0;
    }

    for(i = s; i < q->qend; i += q->qsize)
    {
      if(__sync_bool_compare_and_swap(&q->data[i], MPOOL_FULL, MPOOL_READING))
      {
        q->last_read = i;
        memcpy((char*)p, (char*)q->data+i+1, q->elsize);
        if(swap) memcpy((char*)q->data+i+1, (char*)swap, q->elsize);
        // memory barrier: must read element before setting MPOOL_EMPTY
        __sync_synchronize();
        q->data[i] = MPOOL_EMPTY;
        // __sync_synchronize();
        nocc = __sync_fetch_and_sub(&q->noccupied, 1); // q->noccupied--
        nocc--;
        // __sync_synchronize();

        if(!q->use_spinlock)
        {
          size_t nproducers = q->nproducers - q->sleeping_prods;
          if(q->sleeping_prods &&
             (nproducers < q->nel-nocc || nproducers == 0 || nocc == 0))
          {
            // Notify when space appears in pool or pool empty
            pthread_mutex_lock(&q->read_wait_mutex);
            if(q->sleeping_prods)
              pthread_cond_signal(&q->read_wait_cond); // wake one
            pthread_mutex_unlock(&q->read_wait_mutex);
          }
        }

        return q->elsize;
      }
    }
    s = 0;
  }

  return 0;
}

// if til_empty, wait until pool is empty,
// otherwise wait until space
static inline void _msgpool_wait(MsgPool *q, char til_empty)
{
  // printf("waiting until %s\n", til_empty ? "empty" : "space");
  size_t limit = til_empty ? 0 : q->nel - 1;
  if(q->noccupied > limit) {
    if(q->use_spinlock) {
      while(q->noccupied > limit) {}
    }
    else {
      pthread_mutex_lock(&q->read_wait_mutex);
      __sync_fetch_and_add(&q->sleeping_prods, 1); // atomic q->sleeping_prods++
      while(q->noccupied > limit)
        pthread_cond_wait(&q->read_wait_cond, &q->read_wait_mutex);
      __sync_fetch_and_sub(&q->sleeping_prods, 1); // atomic q->sleeping_prods--
      pthread_mutex_unlock(&q->read_wait_mutex);
    }
  }
  // printf("done waiting %zu\n", q->noccupied);
}

static inline void msgpool_write(MsgPool *q, const void *restrict p,
                                 void *restrict swap)
{
  size_t i, nocc, s = q->last_write;
  while(1)
  {
    // Wait until there is space to write
    _msgpool_wait(q, 0);

    for(i = s; i < q->qend; i += q->qsize)
    {
      if(__sync_bool_compare_and_swap(&q->data[i], MPOOL_EMPTY, MPOOL_WRITING))
      {
        q->last_write = i;
        if(swap) memcpy((char*)swap, (char*)q->data+i+1, q->elsize);
        memcpy((char*)q->data+i+1, (char*)p, q->elsize);
        // memory barrier on writes: must write element before writing status
        // dev: may not be needed on x86, this is already promised
        // __sync_synchronize();
        q->data[i] = MPOOL_FULL;
        // __sync_synchronize();
        nocc = __sync_fetch_and_add(&q->noccupied, 1); // q->noccupied++
        nocc++;
        // __sync_synchronize();

        if(!q->use_spinlock)
        {
          size_t nconsumers = q->nconsumers - q->sleeping_cons;
          if(q->sleeping_cons &&
             (nconsumers < nocc || nconsumers == 0 || q->noccupied == q->nel))
          {
            // Notify waiting readers when element written
            pthread_mutex_lock(&q->write_wait_mutex);
            if(q->sleeping_cons)
              pthread_cond_signal(&q->write_wait_cond); // wake reading thread
            pthread_mutex_unlock(&q->write_wait_mutex);
          }
        }

        return;
      }
    }

    s = 0;
    // printf("Duh!\n");
  }
}

// Wait until the pool is empty, keep msgpool_read() blocking
static inline void msgpool_wait_til_empty(MsgPool *q)
{
  _msgpool_wait(q, 1);
}

// Close causes msgpool_read() to return 0 if pool is empty
static inline void msgpool_close(MsgPool *q)
{
  q->open = 0;
  if(!q->use_spinlock && q->sleeping_cons) {
    pthread_mutex_lock(&q->write_wait_mutex);
    if(q->sleeping_cons)
      pthread_cond_broadcast(&q->write_wait_cond); // wake all sleeping threads
    pthread_mutex_unlock(&q->write_wait_mutex);
  }
}

static inline void msgpool_reopen(MsgPool *q) {
  q->open = 1;
}

#endif /* MSG_POOL_H_ */
