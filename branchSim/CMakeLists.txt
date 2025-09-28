project(branchSim)
add_library(branchSim SHARED branchSim.c)
target_include_directories(branchSim PRIVATE ../common)
