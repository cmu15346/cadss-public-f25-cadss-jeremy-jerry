#include "coher_internal.h"

void sendBusRd(uint64_t addr, int procNum)
{
    inter_sim->busReq(BUSRD, addr, procNum);
}

void sendBusWr(uint64_t addr, int procNum)
{
    inter_sim->busReq(BUSWR, addr, procNum);
}

void sendData(uint64_t addr, int procNum)
{
    if (CADSS_VERBOSE) {
        printf("Processor %d sending DATA for address %lx\n", procNum, addr);
    }
    inter_sim->busReq(DATA, addr, procNum);
}

void indicateShared(uint64_t addr, int procNum)
{
    if (CADSS_VERBOSE) {
        printf("Processor %d indicating SHARED for address %lx\n", procNum, addr);
    }
    inter_sim->busReq(SHARED, addr, procNum);
}

coherence_states
cacheMI(uint8_t is_read, uint8_t* permAvail, coherence_states currentState,
        uint64_t addr, int procNum)
{
    switch (currentState)
    {
        case INVALID:
            *permAvail = 0;
            sendBusWr(addr, procNum);
            return INVALID_MODIFIED;
        case MODIFIED:
            *permAvail = 1;
            return MODIFIED;
        case INVALID_MODIFIED:
            fprintf(stderr, "IM state on %lx, but request %d\n", addr,
                    is_read);
            *permAvail = 0;
            return INVALID_MODIFIED;
        default:
            fprintf(stderr, "State %d not supported, found on %lx\n",
                    currentState, addr);
            break;
    }

    return INVALID;
}

coherence_states
snoopMI(bus_req_type reqType, cache_action* ca, coherence_states currentState,
        uint64_t addr, int procNum)
{
    *ca = NO_ACTION;
    switch (currentState)
    {
        case INVALID:
            return INVALID;
        case MODIFIED:
            sendData(addr, procNum);
            // indicateShared(addr, procNum); // Needed for E state
            *ca = INVALIDATE;
            return INVALID;
        case INVALID_MODIFIED:
            if (reqType == DATA || reqType == SHARED)
            {
                *ca = DATA_RECV;
                return MODIFIED;
            }

            return INVALID_MODIFIED;
        default:
            fprintf(stderr, "State %d not supported, found on %lx\n",
                    currentState, addr);
            break;
    }

    return INVALID;
}

coherence_states
cacheMSI(uint8_t is_read, uint8_t* permAvail, coherence_states currentState,
        uint64_t addr, int procNum)
{
    switch (currentState)
    {
        case INVALID:
            *permAvail = 0;
            if (is_read) {
                sendBusRd(addr, procNum);
                return INVALID_SHARED;
            }
            else {
                sendBusWr(addr, procNum);
                return INVALID_MODIFIED;
            }
        case SHARE:
            if (is_read)
            {
                *permAvail = 1;
                return SHARE;
            }
            else
            {
                *permAvail = 0;
                sendBusWr(addr, procNum);
                return SHARED_MODIFIED;
            }
        case MODIFIED:
            *permAvail = 1;
            return MODIFIED;
        case SHARED_MODIFIED:
            if (is_read)
            {
                *permAvail = 1;
            }
            else
            {
                *permAvail = 0;
            }
            return SHARED_MODIFIED;
        case INVALID_MODIFIED:
            fprintf(stderr, "IM state on %lx, but request %d\n", addr,
                    is_read);
            *permAvail = 0;
            return INVALID_MODIFIED;
        case INVALID_SHARED:
            *permAvail = 0;
            if (is_read)
            {
                return INVALID_SHARED;
            }
            else
            {
                sendBusWr(addr, procNum);
                return INVALID_MODIFIED;
            }
        default:
            fprintf(stderr, "State %d not supported, found on %lx\n",
                    currentState, addr);
            break;
    }

    return INVALID;
}

coherence_states
snoopMSI(bus_req_type reqType, cache_action* ca, coherence_states currentState,
        uint64_t addr, int procNum)
{
    *ca = NO_ACTION;
    switch (currentState)
    {
        case INVALID:
            return INVALID;
        case MODIFIED:
            sendData(addr, procNum);
            // indicateShared(addr, procNum); // Needed for E state
            if (reqType == BUSRD)
                return SHARE;
            else if (reqType == BUSWR) {
                *ca = INVALIDATE;
                return INVALID;
            }
            return MODIFIED;
        case SHARE:
            if (reqType == BUSWR)
            {
                *ca = INVALIDATE;
                return INVALID;
            }
            return SHARE;
        case SHARED_MODIFIED:
            if (reqType == DATA)
            {
                *ca = DATA_RECV;
                return MODIFIED;
            }
            return SHARED_MODIFIED;
        case INVALID_MODIFIED:
            //maybe need to check for addr
            if (reqType == DATA)
            {
                *ca = DATA_RECV;
                return MODIFIED;
            }

            return INVALID_MODIFIED;
        case INVALID_SHARED:
            if (reqType == DATA)
            {
                *ca = DATA_RECV;
                return SHARE;
            }

            return INVALID_SHARED;
        default:
            fprintf(stderr, "State %d not supported, found on %lx\n",
                    currentState, addr);
            break;
    }

    return INVALID;
}

coherence_states
cacheMESI(uint8_t is_read, uint8_t* permAvail, coherence_states currentState,
        uint64_t addr, int procNum)
{
    switch (currentState)
    {
        case INVALID:
            *permAvail = 0;
            if (is_read) {
                sendBusRd(addr, procNum);
                return INVALID_SHARED_EXCLUSIVE;
            }
            else {
                sendBusWr(addr, procNum);
                return INVALID_MODIFIED;
            }
        case SHARE:
            if (is_read)
            {
                *permAvail = 1;
                return SHARE;
            }
            else
            {
                *permAvail = 0;
                sendBusWr(addr, procNum);
                return SHARED_MODIFIED;
            }
        case MODIFIED:
            *permAvail = 1;
            return MODIFIED;
        case EXCLUSIVE:
            *permAvail = 1;
            if (is_read)
            {
                return EXCLUSIVE;
            }
            else
            {
                return MODIFIED;
            }
        case SHARED_MODIFIED:
            if (is_read)
            {
                *permAvail = 1;
            }
            else
            {
                *permAvail = 0;
            }
            return SHARED_MODIFIED;
        case INVALID_MODIFIED:
            fprintf(stderr, "IM state on %lx, but request %d\n", addr,
                    is_read);
            *permAvail = 0;
            return INVALID_MODIFIED;
        case INVALID_SHARED_EXCLUSIVE:
            *permAvail = 0;
            if (is_read)
            {
                return INVALID_SHARED_EXCLUSIVE;
            }
            else
            {
                sendBusWr(addr, procNum);
                return INVALID_MODIFIED;
            }
            return INVALID_SHARED_EXCLUSIVE;
        default:
            fprintf(stderr, "State %d not supported, found on %lx\n",
                    currentState, addr);
            break;
    }

    return INVALID;
}

coherence_states
snoopMESI(bus_req_type reqType, cache_action* ca, coherence_states currentState,
        uint64_t addr, int procNum)
{
    *ca = NO_ACTION;
    switch (currentState)
    {
        case INVALID:
            return INVALID;
        case MODIFIED:
            if (reqType == BUSRD) {
                indicateShared(addr, procNum);
                sendData(addr, procNum);
                return SHARE;
            }
            else if (reqType == BUSWR) {
                sendData(addr, procNum);
                *ca = INVALIDATE;
                return INVALID;
            }
            return MODIFIED;
        case EXCLUSIVE:
            if (reqType == BUSRD) { 
                indicateShared(addr, procNum); 
                return SHARE;
            } 
            else if (reqType == SHARED) {
                return SHARE; 
            } 
            else if (reqType == BUSWR) {
                *ca = INVALIDATE;
                return INVALID;
            }
            return EXCLUSIVE;
        case SHARE:
            if (reqType == BUSWR)
            {
                *ca = INVALIDATE;
                return INVALID;
            }
            if (reqType == BUSRD) {
                indicateShared(addr, procNum);
            }
            return SHARE;
        case SHARED_MODIFIED:
            if (reqType == DATA)
            {
                *ca = DATA_RECV;
                return MODIFIED;
            }
            return SHARED_MODIFIED;
        case INVALID_MODIFIED:
            if (reqType == DATA)
            {
                *ca = DATA_RECV;
                return MODIFIED;
            }

            return INVALID_MODIFIED;
        case INVALID_SHARED_EXCLUSIVE:
            if (reqType == DATA)
            {
                *ca = DATA_RECV;
                return EXCLUSIVE;
            }
            if (reqType == SHARED) {
                *ca = DATA_RECV;
                return SHARE;
            }

            return INVALID_SHARED_EXCLUSIVE;
        default:
            fprintf(stderr, "State %d not supported, found on %lx\n",
                    currentState, addr);
            break;
    }

    return INVALID;
}

coherence_states
cacheMOESI(uint8_t is_read, uint8_t* permAvail, coherence_states currentState,
        uint64_t addr, int procNum)
{
    switch (currentState)
    {
        case INVALID:
            *permAvail = 0;
            if (is_read) {
                sendBusRd(addr, procNum);
                return INVALID_SHARED_EXCLUSIVE;
            }
            else {
                sendBusWr(addr, procNum);
                return INVALID_MODIFIED;
            }
        case OWNED:
            if (is_read)
            {
                *permAvail = 1;
                return OWNED;
            }
            else
            {
                *permAvail = 0;
                sendBusWr(addr, procNum);
                return SHARED_MODIFIED;
            }
        case SHARE:
            if (is_read)
            {
                *permAvail = 1;
                return SHARE;
            }
            else
            {
                *permAvail = 0;
                sendBusWr(addr, procNum);
                return SHARED_MODIFIED;
            }
        case MODIFIED:
            *permAvail = 1;
            return MODIFIED;
        case EXCLUSIVE:
            *permAvail = 1;
            if (is_read)
            {
                return EXCLUSIVE;
            }
            else
            {
                return MODIFIED;
            }
        case SHARED_MODIFIED:
            if (is_read)
            {
                *permAvail = 1;
            }
            else
            {
                *permAvail = 0;
            }
            return SHARED_MODIFIED;
        case INVALID_MODIFIED:
            fprintf(stderr, "IM state on %lx, but request %d\n", addr,
                    is_read);
            *permAvail = 0;
            return INVALID_MODIFIED;
        case INVALID_SHARED_EXCLUSIVE:
            *permAvail = 0;
            if (is_read)
            {
                return INVALID_SHARED_EXCLUSIVE;
            }
            else
            {
                sendBusWr(addr, procNum);
                return INVALID_MODIFIED;
            }
            return INVALID_SHARED_EXCLUSIVE;
        default:
            fprintf(stderr, "State %d not supported, found on %lx\n",
                    currentState, addr);
            break;
    }

    return INVALID;
}

coherence_states
snoopMOESI(bus_req_type reqType, cache_action* ca, coherence_states currentState,
        uint64_t addr, int procNum)
{
    *ca = NO_ACTION;
    switch (currentState)
    {
        case INVALID:
            return INVALID;
        case MODIFIED:
            if (reqType == BUSRD) {
                indicateShared(addr, procNum);
                sendData(addr, procNum);
                return OWNED;
            }
            else if (reqType == BUSWR) {
                sendData(addr, procNum);
                *ca = INVALIDATE;
                return INVALID;
            }
            return MODIFIED;
        case OWNED:
            if (reqType == BUSRD) {
                indicateShared(addr, procNum);
                sendData(addr, procNum);
                return OWNED;
            }
            else if (reqType == BUSWR) {
                sendData(addr, procNum);
                *ca = INVALIDATE;
                return INVALID;
            }
            return OWNED;
        case EXCLUSIVE:
            if (reqType == BUSRD) { 
                indicateShared(addr, procNum); 
                return SHARE;
            } 
            else if (reqType == SHARED) {
                return SHARE; 
            } 
            else if (reqType == BUSWR) {
                *ca = INVALIDATE;
                return INVALID;
            }
            return EXCLUSIVE;
        case SHARE:
            if (reqType == BUSWR)
            {
                *ca = INVALIDATE;
                return INVALID;
            }
            if (reqType == BUSRD) {
                indicateShared(addr, procNum);
            }
            return SHARE;
        case SHARED_MODIFIED:
            if (reqType == DATA)
            {
                *ca = DATA_RECV;
                return MODIFIED;
            }
            return SHARED_MODIFIED;
        case INVALID_MODIFIED:
            if (reqType == DATA)
            {
                *ca = DATA_RECV;
                return MODIFIED;
            }

            return INVALID_MODIFIED;
        case INVALID_SHARED_EXCLUSIVE:
            if (reqType == DATA)
            {
                *ca = DATA_RECV;
                return EXCLUSIVE;
            }
            if (reqType == SHARED) {
                *ca = DATA_RECV;
                return SHARE;
            }

            return INVALID_SHARED_EXCLUSIVE;
        default:
            fprintf(stderr, "State %d not supported, found on %lx\n",
                    currentState, addr);
            break;
    }

    return INVALID;
}

coherence_states
cacheMESIF(uint8_t is_read, uint8_t* permAvail, coherence_states currentState,
        uint64_t addr, int procNum)
{
    switch (currentState)
    {
        case INVALID:
            *permAvail = 0;
            if (is_read) {
                sendBusRd(addr, procNum);
                return INVALID_SHARED_EXCLUSIVE;
            }
            else {
                sendBusWr(addr, procNum);
                return INVALID_MODIFIED;
            }
        case FORWARD:
            if (is_read)
            {
                *permAvail = 1;
                return FORWARD;
            }
            else
            {
                *permAvail = 0;
                sendBusWr(addr, procNum);
                return SHARED_MODIFIED;
            }
        case SHARE:
            if (is_read)
            {
                *permAvail = 1;
                return SHARE;
            }
            else
            {
                *permAvail = 0;
                sendBusWr(addr, procNum);
                return SHARED_MODIFIED;
            }
        case MODIFIED:
            *permAvail = 1;
            return MODIFIED;
        case EXCLUSIVE:
            *permAvail = 1;
            if (is_read)
            {
                return EXCLUSIVE;
            }
            else
            {
                return MODIFIED;
            }
        case SHARED_MODIFIED:
            if (is_read)
            {
                *permAvail = 1;
            }
            else
            {
                *permAvail = 0;
            }
            return SHARED_MODIFIED;
        case INVALID_MODIFIED:
            fprintf(stderr, "IM state on %lx, but request %d\n", addr,
                    is_read);
            *permAvail = 0;
            return INVALID_MODIFIED;
        case INVALID_SHARED_EXCLUSIVE:
            *permAvail = 0;
            if (is_read)
            {
                return INVALID_SHARED_EXCLUSIVE;
            }
            else
            {
                sendBusWr(addr, procNum);
                return INVALID_MODIFIED;
            }
            return INVALID_SHARED_EXCLUSIVE;
        default:
            fprintf(stderr, "State %d not supported, found on %lx\n",
                    currentState, addr);
            break;
    }

    return INVALID;
}

coherence_states
snoopMESIF(bus_req_type reqType, cache_action* ca, coherence_states currentState,
        uint64_t addr, int procNum)
{
    *ca = NO_ACTION;
    switch (currentState)
    {
        case INVALID:
            return INVALID;
        case MODIFIED:
            if (reqType == BUSRD) {
                indicateShared(addr, procNum);
                return SHARE;
            }
            else if (reqType == BUSWR) {
                sendData(addr, procNum);
                *ca = INVALIDATE;
                return INVALID;
            }
            return MODIFIED;
        case FORWARD:
            if (reqType == BUSRD) {
                indicateShared(addr, procNum);
                return SHARE;
            }
            else if (reqType == BUSWR) {
                sendData(addr, procNum);
                *ca = INVALIDATE;
                return INVALID;
            }
            return FORWARD;
        case EXCLUSIVE:
            if (reqType == BUSRD) { 
                indicateShared(addr, procNum); 
                return SHARE;
            } 
            else if (reqType == SHARED) {
                return SHARE; 
            } 
            else if (reqType == BUSWR) {
                *ca = INVALIDATE;
                return INVALID;
            }
            return EXCLUSIVE;
        case SHARE:
            if (reqType == BUSWR)
            {
                *ca = INVALIDATE;
                return INVALID;
            }
            return SHARE;
        case SHARED_MODIFIED:
            if (reqType == DATA)
            {
                *ca = DATA_RECV;
                return MODIFIED;
            }
            return SHARED_MODIFIED;
        case INVALID_MODIFIED:
            if (reqType == DATA)
            {
                *ca = DATA_RECV;
                return MODIFIED;
            }

            return INVALID_MODIFIED;
        case INVALID_SHARED_EXCLUSIVE:
            if (reqType == DATA)
            {
                *ca = DATA_RECV;
                return EXCLUSIVE;
            }
            if (reqType == SHARED) {
                *ca = DATA_RECV;
                return FORWARD;
            }

            return INVALID_SHARED_EXCLUSIVE;
        default:
            fprintf(stderr, "State %d not supported, found on %lx\n",
                    currentState, addr);
            break;
    }

    return INVALID;
}