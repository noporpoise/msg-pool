msg-pool: Multiple Producers / Multiple Consumers Message Passing Pool  
license: Public Domain  
url: https://github.com/noporpoise/msg-pool  
Isaac turner <turner.isaac at gmail.com>  
Jan 2014

Fast message passing
--------------------

High throughput message passing pool mixing lockless pool with blocking mutexes
/ conditional variables when we need to wait for input/output.
This is a *pool* not a *queue* - messages are delivered in no particular order. 
Primary use is for sharing tasks when there are multiple producer threads and
multiple consumer threads.
We do not worry about latency only total throughput rate.

Features/limitations:
* fixed size pool
* messages passed without guarantees about ordering
* multiple producers / multiple consumer threads
* lockless except when pool is full or empty

Example
-------

    MsgPool q;
    msgpool_alloc_spinlock(&q, qlen, sizeof(int));

    // Reader threads
    int read;
    while(msgpool_read(&q, &r, NULL)) printf("Got %i\n", r);

    // Writer threads
    int w = 12;
    msgpool_write(&q, &w, NULL);

    msgpool_dealloc(&q);

API
---

    void msgpool_alloc_spinlock(MsgPool *q, size_t nel, size_t elsize)

Create a new message pool using spinlocks to block. This approach may be fastest
if you have more CPU cores than threads.

    void msgpool_alloc_mutex(MsgPool *q, size_t nel, size_t elsize,
                             size_t nproducers, size_t nconsumers)

Create a new message pool using mutexes to block. You can have more than the
number of producers/consumers that you specify, but if you have fewer you may
affect performance.  This approach is probably faster if you have multiple
threads running per CPU core.

    void msgpool_dealloc(MsgPool *q)

Release a message pool

    void msgpool_iterate(MsgPool *q,
                         void (*func)(void *el, size_t idx, void *args),
                         void *args)

Iterate over elements in the pool. Example:

    void alloc_elements(void *ptr, size_t i, void *args)
    {
      (void)args; (void)i;
      char *tmp = malloc(100);
      memcpy(ptr, &tmp, sizeof(char*));
    }

    MsgPool q;
    msgpool_alloc(&q, qlen, sizeof(char*), nproducers, nconsumers);
    msgpool_iterate(&q, alloc_elements, NULL);

`msgpool_iterate()` is useful for e.g. initialising all elements to pointers to memory.
Beware: elements passed to func() function by msgpool_iterate will not be aligned in
memory.

    int msgpool_read(MsgPool *q, void *restrict p, const void *restrict swap)

Read an element from the pool. `swap` is optional, if non-NULL the data pointed
to by swap is used to overwrite the element after it is read.
Returns: bytes read or 0 on failure.

    void msgpool_write(MsgPool *q, const void *restrict p, void *restrict swap)

Write an element to the pool.  `swap` is optional, if non-NULL the memory pointed
to is overwritten with the data that is about to be overwritten in the pool.

    void msgpool_wait_til_empty(MsgPool *q)

Block until all elements have been read from the pool

    void msgpool_close(MsgPool *q)

Close causes msgpool_read() to return 0 if pool is empty.

    void msgpool_reopen(MsgPool *q)

Reopen the pool for reading after calling close()

License
-------

Public Domain. No warranty. You may use this code as you wish without
restrictions. There are probably some bugs.

Please open an issue on github with ideas, bug reports or feature requests.
