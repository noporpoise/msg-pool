#ifndef MSG_POOL_H_
#define MSG_POOL_H_

#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <pthread.h>
#include <sched.h> // sched_yield()
#include <unistd.h>  // need for getpid()
#include <signal.h> // needed for abort()
#include <assert.h>
#include <errno.h>

#define MPOOL_EMPTY 0
#define MPOOL_CLAIMED 1
#define MPOOL_FULL 2

#define MSGP_LOCK_SPIN 0
#define MSGP_LOCK_YIELD 1
#define MSGP_LOCK_MUTEX 2

// 1. merge claim_read / claim_write (?)
// 2. Use rand odd numbers to iterate list to avoid repeatedly clashing (?)

typedef struct
{
  // qsize = elsize+1, qend=(qsize)*nel
  const size_t nel, elsize, qsize, qend;

  volatile size_t num_full, num_empty, num_waiting_readers, num_waiting_writers;
  volatile size_t last_read, last_write;
  char *const data; // [<state><element>]+

  // reads block until success if open is != 0
  // otherwise they return 0
  volatile char open;

  // Blocking / locking mechanism (SPIN,YIELD,MUTEX)
  const char locking;

  // Mutexes
  pthread_mutex_t reader_wait_mutex, writer_wait_mutex;
  pthread_cond_t reader_wait_cond, writer_wait_cond;
} MsgPool;

#define msgpool_get_ptr(pool,pos) ((void*)((pool)->data+(pos)+1))

#define msgpool_error(fmt,...) do { \
  fprintf(stderr, fmt, __VA_ARGS__); \
  abort(); \
} while(0)

static inline void msgpool_alloc(MsgPool *q, size_t nel, size_t elsize,
                                 char locking)
{
  if(locking != MSGP_LOCK_SPIN && locking != MSGP_LOCK_YIELD &&
     locking != MSGP_LOCK_MUTEX) {
    msgpool_error("[%s:%i] Invalid locking param\n", __FILE__, __LINE__);
  }

  // 1 byte per element for locking
  char *data = calloc(nel, elsize+1);
  MsgPool tmpq = {.nel = nel, .elsize = elsize, .qsize = elsize+1,
                  .qend = (elsize+1)*nel, .data = data,
                  .num_full = 0, .num_empty = nel,
                  .num_waiting_readers = 0, .num_waiting_writers = 0,
                  .open = true,
                  .last_read = 0, .last_write = 0,
                  .locking = locking};

  memcpy(q, &tmpq, sizeof(MsgPool));

  if(pthread_mutex_init(&q->reader_wait_mutex, NULL) != 0 ||
     pthread_mutex_init(&q->writer_wait_mutex, NULL) != 0)
  {
    msgpool_error("pthread_mutex init failed: %s\n", strerror(errno));
  }

  if(pthread_cond_init(&q->reader_wait_cond, NULL) != 0 ||
     pthread_cond_init(&q->writer_wait_cond, NULL) != 0)
  {
    msgpool_error("pthread_cond init failed: %s\n", strerror(errno));
  }
}

#define msgpool_alloc_spinlock(q,n,size) msgpool_alloc(q,n,size,MSGP_LOCK_SPIN)
#define msgpool_alloc_yield(q,n,size) msgpool_alloc(q,n,size,MSGP_LOCK_YIELD)
#define msgpool_alloc_mutex(q,n,size) msgpool_alloc(q,n,size,MSGP_LOCK_MUTEX)

// Deallocate a new pool
static inline void msgpool_dealloc(MsgPool *q)
{
  pthread_cond_destroy(&q->reader_wait_cond);
  pthread_mutex_destroy(&q->reader_wait_mutex);
  pthread_cond_destroy(&q->writer_wait_cond);
  pthread_mutex_destroy(&q->writer_wait_mutex);
  free(q->data);
}

// Iterate over elements in the pool
// calls func(el,idx,args) with idx being 0,1,2... and el a pointer to the
// element (beware: not aligned in memory)
// Can be used to initialise elements at the begining or clean up afterwards
static inline void msgpool_iterate(MsgPool *q,
                                   void (*func)(void *el, size_t idx, void *args),
                                   void *args)
{
  size_t i; char *ptr, *data = q->data;
  for(i = 0, ptr = data+1; i < q->nel; i++, ptr += q->qsize) {
    func((void*)ptr, i, args);
  }
}

// if til_empty, wait until pool is empty,
// otherwise wait until space
static inline void _msgpool_wait_for_empty(MsgPool *q, bool til_empty)
{
  // printf("waiting until %s\n", til_empty ? "empty" : "space");
  size_t limit = til_empty ? q->nel : 1;
  if(q->num_empty < limit)
  {
    switch(q->locking) {
      case MSGP_LOCK_SPIN:
        while(q->num_empty < limit) {}
        break;
      case MSGP_LOCK_YIELD:
        while(q->num_empty < limit)
          if(sched_yield()) msgpool_error("yield failed: %s", strerror(errno));
        break;
      case MSGP_LOCK_MUTEX:
        if(sched_yield()) msgpool_error("yield failed: %s", strerror(errno));
        if(q->num_empty >= limit) break;
        pthread_mutex_lock(&q->writer_wait_mutex);
        q->num_waiting_writers++;
        while(q->num_empty < limit)
          pthread_cond_wait(&q->writer_wait_cond, &q->writer_wait_mutex);
        q->num_waiting_writers--;
        pthread_mutex_unlock(&q->writer_wait_mutex);
        break;
    }
  }
}

// Wait until there is at least one element in the pool or it is closed
static inline void _msgpool_wait_for_full(MsgPool *q)
{
  if(q->num_full == 0 && q->open)
  {
    // Wait on write
    switch(q->locking) {
      case MSGP_LOCK_SPIN:
        while(q->num_full == 0 && q->open) {}
        break;
      case MSGP_LOCK_YIELD:
        // sched_yield returns non-zero on error
        while(q->num_full == 0 && q->open)
          if(sched_yield()) msgpool_error("yield failed: %s", strerror(errno));
        break;
      case MSGP_LOCK_MUTEX:
        if(sched_yield()) msgpool_error("yield failed: %s", strerror(errno));
        if(q->num_full > 0 || !q->open) break;
        // don't need to use __sync_fetch_and_add because we have writer_wait_mutex
        pthread_mutex_lock(&q->reader_wait_mutex);
        q->num_waiting_readers++;
        while(q->num_full == 0 && q->open)
          pthread_cond_wait(&q->reader_wait_cond, &q->reader_wait_mutex);
        q->num_waiting_readers--;
        pthread_mutex_unlock(&q->reader_wait_mutex);
        break;
    }
  }
}

// Returns index claimed or -1 if msgpool is closed
static inline int msgpool_claim_read(MsgPool *q)
{
  size_t i, s = q->last_read;

  while(1)
  {
    _msgpool_wait_for_full(q);

    if(q->num_full == 0 && !q->open) return -1;

    for(i = s; i < q->qend; i += q->qsize)
    {
      if(q->data[i] == MPOOL_FULL &&
         __sync_bool_compare_and_swap((volatile char*)&q->data[i],
                                      MPOOL_FULL, MPOOL_CLAIMED))
      {
        q->last_read = i;
        __sync_sub_and_fetch(&q->num_full, 1); // q->num_full--;
        return (int)i;
      }
    }

    s = 0;
  }
}

// returns index
static inline int msgpool_claim_write(MsgPool *q)
{
  size_t i, s = q->last_write;

  while(1)
  {
    // Wait until there is space to write
    _msgpool_wait_for_empty(q, false);

    for(i = s; i < q->qend; i += q->qsize)
    {
      if(q->data[i] == MPOOL_EMPTY &&
         __sync_bool_compare_and_swap((volatile char*)&q->data[i],
                                      MPOOL_EMPTY, MPOOL_CLAIMED))
      {
        q->last_write = i;
        __sync_sub_and_fetch(&q->num_empty, 1); // q->num_empty--;
        return (int)i;
      }
    }

    s = 0;
  }
}

// new_state must be MPOOL_EMPTY or MPOOL_FULL
static inline void msgpool_release(MsgPool *q, size_t pos, char new_state)
{
  assert(new_state == MPOOL_EMPTY || new_state == MPOOL_FULL);
  assert(q->data[pos] == MPOOL_CLAIMED);

  __sync_synchronize();
  q->data[pos] = new_state;

  if(new_state == MPOOL_EMPTY)
  {
    __sync_add_and_fetch(&q->num_empty, 1);

    if(q->locking == MSGP_LOCK_MUTEX && q->num_waiting_writers)
    {
      // Notify when space appears in pool or pool empty
      pthread_mutex_lock(&q->writer_wait_mutex);
      if(q->num_waiting_writers) pthread_cond_signal(&q->writer_wait_cond);
      pthread_mutex_unlock(&q->writer_wait_mutex);
    }
  }
  else
  {
    // MPOOL_FULL
    __sync_add_and_fetch(&q->num_full, 1);

    if(q->locking == MSGP_LOCK_MUTEX && q->num_waiting_readers)
    {
      // Notify when space appears in pool or pool empty
      pthread_mutex_lock(&q->reader_wait_mutex);
      if(q->num_waiting_readers) pthread_cond_signal(&q->reader_wait_cond);
      pthread_mutex_unlock(&q->reader_wait_mutex);
    }
  }
}

static inline void msgpool_read(MsgPool *pool, void *restrict ptr,
                                void *restrict swap)
{
  int pos = msgpool_claim_read(pool);
  memcpy(ptr, msgpool_get_ptr(pool, pos), pool->elsize);
  if(swap) memcpy(msgpool_get_ptr(pool, pos), swap, pool->elsize);
  msgpool_release(pool, pos, MPOOL_EMPTY);
}

static inline void msgpool_write(MsgPool *pool, void *restrict ptr,
                                void *restrict swap)
{
  int pos = msgpool_claim_write(pool);
  if(swap) memcpy(swap, msgpool_get_ptr(pool, pos), pool->elsize);
  memcpy(msgpool_get_ptr(pool, pos), ptr, pool->elsize);
  msgpool_release(pool, pos, MPOOL_FULL);
}

// Close causes msgpool_read() to return 0 if pool is empty
// Beware: this function doesn't block until the pool is emtpy
//         for that call msgpool_wait_til_empty(q) after calling msgpool_close(q)
static inline void msgpool_close(MsgPool *q)
{
  q->open = 0;
  if(q->locking == MSGP_LOCK_MUTEX && q->num_waiting_readers) {
    pthread_mutex_lock(&q->reader_wait_mutex);
    if(q->num_waiting_readers)
      pthread_cond_broadcast(&q->reader_wait_cond); // wake all sleeping threads
    pthread_mutex_unlock(&q->reader_wait_mutex);
  }
}

static inline void msgpool_reopen(MsgPool *q) {
  q->open = 1;
}

// Wait until the pool is empty, keep msgpool_read() blocking
static inline void msgpool_wait_til_empty(MsgPool *q)
{
  _msgpool_wait_for_empty(q, true);
}

#endif /* MSG_POOL_H_ */
