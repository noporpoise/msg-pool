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

Example
-------

    MsgPool q;
    msgpool_alloc(&q, qlen, sizeof(size_t), nproducers, nconsumers);

    // Reader threads
    int read;
    msgpool_read(&q, &r, NULL);

    // Writer threads
    int w = 12;
    msgpool_write(&q, &w, NULL);

    msgpool_dealloc(&q);

API
---

    void msgpool_alloc(MsgPool *q, size_t nel, size_t elsize,
                    size_t nproducers, size_t nconsumers)

Create a new message pool

    void msgpool_dealloc(MsgPool *q)

Release a message pool

    void msgpool_init(MsgPool *q,
                   void (*func)(char *el, size_t idx, void *args),
                   void *args)

Set the initial value of elements in the pool. Example:

    void alloc_elements(char *ptr, size_t i, void *args)
    {
      (void)args; (void)i;
      char *tmp = malloc(100);
      memcpy(ptr, &tmp, sizeof(size_t));
    }

    MsgPool q;
    msgpool_alloc(&q, qlen, sizeof(char*), nproducers, nconsumers);
    msgpool_init(&q, alloc_elements, NULL);

`msgpool_init()` is useful for e.g. initialising all elements to pointers to memory.
Beware: elements passed to func() function by msgpool_init will not be aligned in
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

Public Domain. No warranty. There are probably some bugs.

Please open an issue on github with ideas, bug reports or feature requests.
