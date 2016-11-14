# Project 3 Crawler

# Code Explanation

The code conforms to the following spec (copied from the project3a description).

crawl() will return 0 on success and -1 on failure.
The arguments are as follows:
- start_url: the first link to visit. All other links will be discovered by following links out
recursively from this first page.
- download_workers: the number of worker threads in the download pool.
- parse_workers: the number of workers threads in the parse pool.
- queue_size: the size of the links queue (the downloaders pop off this queue to get more
work).
- fetch_fn: a callback to ask the program using the library to fetch the content to which the
link refers. The function will return NULL if the content isn't reachable (a broken link).
Otherwise, the function will malloc space for the return value, so you must free it when
you're done.
- edge_fn: this is to notify the program that the library encountered a link on the from page
to the to page.

Some caveats.
If download_workers or parse_workers <= 0, they default to 1.
If queue_size <= 0, it defaults to 1000.

## Overview

crawl creates three queues when called.
- finished (return value and id of threads)
- content (queue containing pages that has not yet been parsed)
- link (queue containing links found on a page)

crawl also creates a hash_table to keep track of links that have been seen.

crawl creates parsers and downloaders. Downloaders wait on the link queue to have links. When a link is enqueued, downloaders dequeue the link and set a flag that they have begun downloading the link. When content is downloaded, downloaders add the page to the content queue and remove the flag. Parsers follow a similar method. Parsers dequeue content, and enqueue links. While searching for links on a page, parsers set a flag as well. Parsers and Downloaders work until there is nothing in either queue and no flag is set.

## Multi Threaded Model.
Crawl does not exit until all threads are joined. This requires some Bookkeeping. All threads are assigned an id.

Parsing Threads: 0 < parse_workers
Downloading Threads: parse_workers <= (parse_workers + download_workers)

Threads can enter be in 3 possible states. 0 (not started), 1 (started), 2 (joined).

When a thread finishes running, it adds its return value and its id to a global "finished" queue. After spawning all threads, or canceling the spawning of thread, crawl will exhaust the "finished" queue, join threads by their id, and exit. If any thread has a non 0 return value, crawl will forcibly cancel the remaining threads and exit with -1.

Threads themselves do not exit until there is nothing in either the content queue or the link queue and no flag is set. The flags are semaphores. When a downloader or parser is doing work, they **post** to the semaphore increasing it from its base value of 0. They **wait** on the semaphore when done. When the flags have a value of 0, the program knows that no thread is doing work.

## Queue

I've implemented a simple double ended queue. Enqueueing adds to the tail of the queue, dequeueing, the head. The actual enqueue and dequeue operations are controlled by a lock. The queue also has a "full" and "empty" semaphore for enforcing two rules.

1. The queue cannot exceed its queue_size.
2. The queue cannot be dequeued when empty.

The "full" semaphore is the number of elements in the queue.
The "empty" semaphore is the maximum number of elements in the queue - the number of elements in the queue.

Before enqueueing, the queue waits on the empty semaphore. Afterwards it posts to the full semaphore.

Before dequeueing, the queue waits on the full semaphore. Afterwards it posts to the empty semaphore.

By waiting on semaphores, threads block until they can enqueue or dequeue.

The dequeue operation is special as it is timed to prevent deadlock. There is no way to know beforehand how many elements are in a queue AND how many threads would like to know how many elements are in a queue at a given time. So a thread will attempt to dequeue for 5 ms before giving up. The exception is the "finished" queue. Only one thread "crawl" will be checking its size through the program lifetime. "crawl" waits as long as necessary on the finished queue's dequeue operation.

## Hash Table.

An extremely simple hash table was implemented for this project. The hash function used is fletcher16. It is a dynamically expanding hash_table, which doubles in size when full.

# Results of running valgrind's memcheck

```
valgrind --tool=memcheck --leak-check=full --track-origins=yes ./file_tester pagea
==4586== Memcheck, a memory error detector
==4586== Copyright (C) 2002-2015, and GNU GPL'd, by Julian Seward et al.
==4586== Using Valgrind-3.11.0 and LibVEX; rerun with -h for copyright info
==4586== Command: ./file_tester pagea
==4586==
pagea -> pageb
pagea -> pagec
pagec -> paged
==4586==
==4586== HEAP SUMMARY:
==4586==     in use at exit: 18,656 bytes in 97 blocks
==4586==   total heap usage: 191 allocs, 94 frees, 23,015 bytes allocated
==4586==
==4586== LEAK SUMMARY:
==4586==    definitely lost: 0 bytes in 0 blocks
==4586==    indirectly lost: 0 bytes in 0 blocks
==4586==      possibly lost: 0 bytes in 0 blocks
==4586==    still reachable: 18,656 bytes in 97 blocks
==4586==         suppressed: 0 bytes in 0 blocks
==4586== Reachable blocks (those to which a pointer was found) are not shown.
==4586== To see them, rerun with: --leak-check=full --show-leak-kinds=all
==4586==
==4586== For counts of detected and suppressed errors, rerun with: -v
==4586== ERROR SUMMARY: 0 errors from 0 contexts (suppressed: 0 from 0)
```

Valgrind has a strict definition of __memory leak__. The loose definition of
__memory leak__ is "Memory was allocated and was not freed before the program terminated." This is reflected in the "x lost:" summary. This does not happen in our code. The stricter definition is "Memory was allocated and cannot be freed because the program no longer has pointers to the allocated memory block.". Our __still reachable__ memory leak refers to blocks that could have been freed because the program was still keeping track of the pointers to those blocks. We did not worry about this leak because that memory will be freed when the OS collects the process memory upon exit. Until the program exits, we did not want to free that memory.
