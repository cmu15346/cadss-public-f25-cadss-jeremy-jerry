project(simProcessor)
add_library(simProcessor SHARED simProcessor.c)
target_include_directories(simProcessor PRIVATE ../common)
