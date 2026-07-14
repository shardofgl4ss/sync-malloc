# Sync Malloc

My own custom allocator just for fun. This time doing it right. 
To use, just `LD_PRELOAD=/path/to/libmalloc.so` the library from anywhere after compiling, onto any executable.
To use in your own projects, just link to the library and include `sync_malloc.h`

If you see any non-glibc behavior (even obscure glibc behavior) of the allocator that isn't internal and may effect programs, please let me know, as I am not sure I got truly every behavior or quirk implemented.
