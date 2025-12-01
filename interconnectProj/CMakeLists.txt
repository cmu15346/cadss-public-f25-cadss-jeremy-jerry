project(interconnectProj)
add_library(interconnectProj SHARED interconnectProj.c)
target_include_directories(interconnectProj PRIVATE ../common)
