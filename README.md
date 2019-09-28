# pmalloc
A poor mans memory allocator using a simple freelist.
This is not thread safe.
Fragmentation is a problem.

```
darwin:pmalloc arakshit$ make clean; make
rm pmalloc
clang -Wall -g pmalloc.c -o pmalloc
darwin:pmalloc arakshit$ ./pmalloc&
[1] 7572
darwin:pmalloc arakshit$ tail pm_log
pm: [freeblk no. 2291] [size 413825]
pm: [freeblk no. 2292] [size 394100]
pm: [freeblk no. 2293] [size 103634]
pm: [freeblk no. 2294] [size 534786]
pm: [freeblk no. 2295] [size 616460]
pm: [freeblk no. 2296] [size 325101]
pm: [freeblk no. 2297] [size 8913]
pm: [freeblk no. 2298] [size 275760]
pm: [freeblk no. 2299] [size 34669]
pm: [freeblk no. 2
```
