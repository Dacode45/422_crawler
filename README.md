# Project 3 Crawler

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
