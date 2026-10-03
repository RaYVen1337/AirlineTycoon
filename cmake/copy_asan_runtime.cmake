cmake_minimum_required(VERSION 3.5)

# Usage: cmake -DSRC_DIR=<dir with clang_rt.asan*.dll> -DDST_DIR=<exe dir> -P copy_asan_runtime.cmake
file(GLOB asan_dlls "${SRC_DIR}/clang_rt.asan*.dll")
foreach(dll ${asan_dlls})
    file(COPY "${dll}" DESTINATION "${DST_DIR}")
endforeach()
