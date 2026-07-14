cmake_minimum_required(VERSION 4.2)
project(malloc C)

set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_FLAGS ${CMAKE_C_FLAGS} "-Wall -Wextra -fno-omit-frame-pointer -ffreestanding -Wno-format-security -Wshadow -Werror=strict-prototypes -Werror=implicit -Werror=return-type -Werror=incompatible-pointer-types -Wfloat-conversion -Wno-ignored-attributes")
set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} -ggdb3 -O0")
# The allocator won't function right if -fno-strict-aliasing is removed on -O3 optimizations.
set(CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE} -O3 -fno-strict-aliasing -march=native -mtune=native")
set(CMAKE_C_EXTENSIONS ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

add_library(malloc SHARED sync_malloc.c
            types.h)
add_executable(tester tester.c)
add_executable(stdtest stdtest.c)

set_target_properties(malloc PROPERTIES C_VISIBILITY_PRESET hidden)
target_link_libraries(tester malloc)
