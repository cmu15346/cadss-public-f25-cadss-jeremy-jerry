project(cacheSim)
add_library(cacheSim SHARED cacheSim.c)
target_include_directories(cacheSim PRIVATE ../common)
