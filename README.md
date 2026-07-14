# Sync Malloc

My own custom allocator just for fun. This time doing it right. 
To use, just `LD_PRELOAD=/path/to/libmalloc.so` the library from anywhere after compiling, onto any executable.

If you see any non-glibc behavior of the allocator that isn't internal and may effect programs, please let me know.
