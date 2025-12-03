#ifndef INTERCONNECT_H
#define INTERCONNECT_H
#include <stdint.h>
#include <stdbool.h>

struct _coher;
struct _memory;

typedef enum _bus_req_type
{
    NO_REQ,
    BUSRD,
    BUSWR,
    DATA,
    SHARED,
    MEMORY,
    ACK,
    SHARED_DATA
} bus_req_type;

#include "coherence.h"
#include "memory.h"
#include "common.h"

typedef struct _inter_sim_args {
    int arg_count;
    char** arg_list;
    struct _memory* memory;
} inter_sim_args;

typedef struct _interconn {
    sim_interface si;
    void (*req)(bus_req_type brt, uint64_t addr, int procNum, int pDest, bool broadcast, int msgNum);
    void (*registerCoher)(struct _coher* coherComp);
    int (*busReqCacheTransfer)(uint64_t addr, int procNum);
    debug_env_vars dbgEnv;
} interconn;

#endif
