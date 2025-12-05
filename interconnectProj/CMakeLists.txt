project(interconnectProj)
add_library(interconnectProj SHARED interconnectProj.c stree.c)
target_include_directories(interconnectProj PRIVATE ../common)
