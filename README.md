mpmc_pool: Multiple Producers / Multiple Consumers Pool

High throughput message passing pool mixing lockless pool with blocking mutexes
when we need to wait for input/output.  This is a *pool* not a *pool* -
messages are delivered in no particular order.  Primary use is for sharing
tasks when there are multiple producer threads and multiple consumer threads.
We do not worry about latency only total throughput rate.

Example
-------

    MPMCPool q;
    mpmc_alloc(&q, qlen, sizeof(size_t), nproducers, nconsumers);

    // Reader threads
    int read;
    mpmc_read(&q, &r, NULL);

    // Writer threads
    int w = 12;
    mpmc_write(&q, &w, NULL);

    mpmc_dealloc(&q);

API
---

    void mpmc_alloc(MPMCPool *q, size_t nel, size_t elsize,
                    size_t nproducers, size_t nconsumers)

Create a new message pool

    void mpmc_dealloc(MPMCPool *q)

Release a message pool

    void mpmc_init(MPMCPool *q,
                   void (*func)(char *el, size_t idx, void *args),
                   void *args)

Set the initial value of elements in the pool. Example:

    void alloc_elements(char *ptr, size_t i, void *args)
    {
      (void)args; (void)i;
      char *tmp = malloc(100);
      memcpy(ptr, &tmp, sizeof(size_t));
    }

    MPMCPool q;
    mpmc_alloc(&q, qlen, sizeof(char*), nproducers, nconsumers);
    mpmc_init(&q, alloc_elements, NULL);

`mpmc_init()` is useful for e.g. initialising all elements to pointers to memory.
Beware: elements passed to func() function by mpmc_init will not be aligned in
memory.

    int mpmc_read(MPMCPool *q, void *restrict p, const void *restrict swap)

Read an element from the pool. `swap` is optional, if non-NULL the data pointed
to by swap is used to overwrite the element after it is read.
Returns: bytes read or 0 on failure.

    void mpmc_write(MPMCPool *q, const void *restrict p, void *restrict swap)

Write an element to the pool.  `swap` is optional, if non-NULL the memory pointed
to is overwritten with the data that is about to be overwritten in the pool.

    void mpmc_wait_til_empty(MPMCPool *q)

Block until all elements have been read from the pool

    void mpmc_close(MPMCPool *q)

Close causes mpmc_read() to return 0 if pool is empty.

    void mpmc_reopen(MPMCPool *q)

Reopen the pool for reading after calling close()
