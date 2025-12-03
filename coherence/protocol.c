#include "coher_internal.h"

void sendBusRd(uint64_t addr, int procNum)
{
    if (CADSS_VERBOSE) {
        printf("Processor %d sending BUSRD for address %lx\n", procNum, addr);
    }
    inter_sim->req(BUSRD, addr, procNum, -1, true, -1);
}

void sendBusWr(uint64_t addr, int procNum)
{
    if (CADSS_VERBOSE) {
        printf("Processor %d sending BUSWR for address %lx\n", procNum, addr);
    }
    inter_sim->req(BUSWR, addr, procNum, -1, true, -1);
}

void sendData(uint64_t addr, int procNum, int pDest, int msgNum)
{
    if (CADSS_VERBOSE) {
        printf("Processor %d sending DATA for address %lx\n", procNum, addr);
    }
    inter_sim->req(DATA, addr, procNum, pDest, false, msgNum);
}

void indicateShared(uint64_t addr, int procNum, int pDest, int msgNum)
{
    if (CADSS_VERBOSE) {
        printf("Processor %d indicating SHARED for address %lx\n", procNum, addr);
    }
    inter_sim->req(SHARED, addr, procNum, pDest, false, msgNum);
}

void ack(uint64_t addr, int procNum, int pDest, bus_req_type reqType, int msgNum)
{
    if (reqType != BUSRD && reqType != BUSWR) {
        // Only send ACKs for BusRd and BusWr requests
        return;
    }
    if (CADSS_VERBOSE) {
        printf("Processor %d sending ACK for address %lx\n", procNum, addr);
    }
    inter_sim->req(ACK, addr, procNum, pDest, false, msgNum);
}

void shareData(uint64_t addr, int procNum, int pDest, int msgNum)
{
    if (CADSS_VERBOSE) {
        printf("Processor %d sending SHARED_DATA for address %lx\n", procNum, addr);
    }
    inter_sim->req(SHARED_DATA, addr, procNum, pDest, false, msgNum);
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
        uint64_t addr, int procNum, int srcProc, int msgNum)
{
    *ca = NO_ACTION;
    switch (currentState)
    {
        case INVALID:
            ack(addr, procNum, srcProc, reqType, msgNum);
            return INVALID;
        case MODIFIED:
            sendData(addr, procNum, srcProc, msgNum);
            // indicateShared(addr, procNum, srcProc, msgNum); // Needed for E state
            *ca = INVALIDATE;
            return INVALID;
        case INVALID_MODIFIED:
            ack(addr, procNum, srcProc, reqType, msgNum);
            if (reqType == DATA || reqType == SHARED)
            {
                *ca = DATA_RECV;
                return MODIFIED;
            }

            return INVALID_MODIFIED;
        default:
            fprintf(stderr, "State %d not supported, found on %lx\n",
                    currentState, addr);
            perror("snoopMI: Unsupported state\n");
            exit(1);
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
        uint64_t addr, int procNum, int srcProc, int msgNum)
{
    *ca = NO_ACTION;
    switch (currentState)
    {
        case INVALID:
            ack(addr, procNum, srcProc, reqType, msgNum);
            return INVALID;
        case MODIFIED:
            sendData(addr, procNum, srcProc, msgNum);
            // indicateShared(addr, procNum, srcProc, msgNum); // Needed for E state
            if (reqType == BUSRD)
                return SHARE;
            else if (reqType == BUSWR) {
                *ca = INVALIDATE;
                return INVALID;
            }
            return MODIFIED;
        case SHARE:
            ack(addr, procNum, srcProc, reqType, msgNum);
            if (reqType == BUSWR)
            {
                *ca = INVALIDATE;
                return INVALID;
            }
            return SHARE;
        case SHARED_MODIFIED:
            ack(addr, procNum, srcProc, reqType, msgNum);
            if (reqType == DATA)
            {
                *ca = DATA_RECV;
                return MODIFIED;
            }
            return SHARED_MODIFIED;
        case INVALID_MODIFIED:
            ack(addr, procNum, srcProc, reqType, msgNum);
            //maybe need to check for addr
            if (reqType == DATA)
            {
                *ca = DATA_RECV;
                return MODIFIED;
            }

            return INVALID_MODIFIED;
        case INVALID_SHARED:
            ack(addr, procNum, srcProc, reqType, msgNum);
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
        uint64_t addr, int procNum, int srcProc, int msgNum)
{
    *ca = NO_ACTION;
    switch (currentState)
    {
        case INVALID:
            ack(addr, procNum, srcProc, reqType, msgNum);
            return INVALID;
        case MODIFIED:
            if (reqType == BUSRD) {
                shareData(addr, procNum, srcProc, msgNum);
                return SHARE;
            }
            else if (reqType == BUSWR) {
                sendData(addr, procNum, srcProc, msgNum);
                *ca = INVALIDATE;
                return INVALID;
            }
            ack(addr, procNum, srcProc, reqType, msgNum);
            return MODIFIED;
        case EXCLUSIVE:
            if (reqType == BUSRD) { 
                indicateShared(addr, procNum, srcProc, msgNum); 
                return SHARE;
            } 
            else if (reqType == SHARED) {
                ack(addr, procNum, srcProc, reqType, msgNum);
                return SHARE; 
            } 
            else if (reqType == BUSWR) {
                *ca = INVALIDATE;
                ack(addr, procNum, srcProc, reqType, msgNum);
                return INVALID;
            }
            ack(addr, procNum, srcProc, reqType, msgNum);
            return EXCLUSIVE;
        case SHARE:
            if (reqType == BUSWR)
            {
                ack(addr, procNum, srcProc, reqType, msgNum);
                *ca = INVALIDATE;
                return INVALID;
            }
            if (reqType == BUSRD) {
                indicateShared(addr, procNum, srcProc, msgNum);
            }
            ack(addr, procNum, srcProc, reqType, msgNum);
            return SHARE;
        case SHARED_MODIFIED:
            ack(addr, procNum, srcProc, reqType, msgNum);
            if (reqType == DATA)
            {
                *ca = DATA_RECV;
                return MODIFIED;
            }
            return SHARED_MODIFIED;
        case INVALID_MODIFIED:
            ack(addr, procNum, srcProc, reqType, msgNum);
            if (reqType == DATA)
            {
                *ca = DATA_RECV;
                return MODIFIED;
            }

            return INVALID_MODIFIED;
        case INVALID_SHARED_EXCLUSIVE:
            ack(addr, procNum, srcProc, reqType, msgNum);
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
        uint64_t addr, int procNum, int srcProc, int msgNum)
{
    *ca = NO_ACTION;
    switch (currentState)
    {
        case INVALID:
            ack(addr, procNum, srcProc, reqType, msgNum);
            return INVALID;
        case MODIFIED:
            if (reqType == BUSRD) {
                shareData(addr, procNum, srcProc, msgNum);
                return OWNED;
            }
            else if (reqType == BUSWR) {
                sendData(addr, procNum, srcProc, msgNum);
                *ca = INVALIDATE;
                return INVALID;
            }
            ack(addr, procNum, srcProc, reqType, msgNum);
            return MODIFIED;
        case OWNED:
            if (reqType == BUSRD) {
                shareData(addr, procNum, srcProc, msgNum);
                return OWNED;
            }
            else if (reqType == BUSWR) {
                sendData(addr, procNum, srcProc, msgNum);
                *ca = INVALIDATE;
                return INVALID;
            }
            ack(addr, procNum, srcProc, reqType, msgNum);
            return OWNED;
        case EXCLUSIVE:
            if (reqType == BUSRD) { 
                indicateShared(addr, procNum, srcProc, msgNum); 
                return SHARE;
            } 
            else if (reqType == SHARED) {
                ack(addr, procNum, srcProc, reqType, msgNum);
                return SHARE; 
            } 
            else if (reqType == BUSWR) {
                ack(addr, procNum, srcProc, reqType, msgNum);
                *ca = INVALIDATE;
                return INVALID;
            }
            ack(addr, procNum, srcProc, reqType, msgNum);
            return EXCLUSIVE;
        case SHARE:
            if (reqType == BUSWR)
            {
                *ca = INVALIDATE;
                ack(addr, procNum, srcProc, reqType, msgNum);
                return INVALID;
            }
            if (reqType == BUSRD) {
                indicateShared(addr, procNum, srcProc, msgNum);
                return SHARE;
            }
            ack(addr, procNum, srcProc, reqType, msgNum);
            return SHARE;
        case SHARED_MODIFIED:
            ack(addr, procNum, srcProc, reqType, msgNum);
            if (reqType == DATA)
            {
                *ca = DATA_RECV;
                return MODIFIED;
            }
            return SHARED_MODIFIED;
        case INVALID_MODIFIED:
            ack(addr, procNum, srcProc, reqType, msgNum);
            if (reqType == DATA)
            {
                *ca = DATA_RECV;
                return MODIFIED;
            }

            return INVALID_MODIFIED;
        case INVALID_SHARED_EXCLUSIVE:
            ack(addr, procNum, srcProc, reqType, msgNum);
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
        uint64_t addr, int procNum, int srcProc, int msgNum)
{
    *ca = NO_ACTION;
    switch (currentState)
    {
        case INVALID:
            ack(addr, procNum, srcProc, reqType, msgNum);
            return INVALID;
        case MODIFIED:
            if (reqType == BUSRD) {
                shareData(addr, procNum, srcProc, msgNum);
                return SHARE;
            }
            else if (reqType == BUSWR) {
                sendData(addr, procNum, srcProc, msgNum);
                *ca = INVALIDATE;
                return INVALID;
            }
            ack(addr, procNum, srcProc, reqType, msgNum);
            return MODIFIED;
        case FORWARD:
            if (reqType == BUSRD) {
                shareData(addr, procNum, srcProc, msgNum);
                return SHARE;
            }
            else if (reqType == BUSWR) {
                sendData(addr, procNum, srcProc, msgNum);
                *ca = INVALIDATE;
                return INVALID;
            }
            ack(addr, procNum, srcProc, reqType, msgNum);
            return FORWARD;
        case EXCLUSIVE:
            if (reqType == BUSRD) { 
                indicateShared(addr, procNum, srcProc, msgNum); 
                return SHARE;
            } 
            else if (reqType == SHARED) {
                ack(addr, procNum, srcProc, reqType, msgNum);
                return SHARE; 
            } 
            else if (reqType == BUSWR) {
                ack(addr, procNum, srcProc, reqType, msgNum);
                *ca = INVALIDATE;
                return INVALID;
            }
            ack(addr, procNum, srcProc, reqType, msgNum);
            return EXCLUSIVE;
        case SHARE:
            ack(addr, procNum, srcProc, reqType, msgNum);
            if (reqType == BUSWR)
            {
                *ca = INVALIDATE;
                return INVALID;
            }
            return SHARE;
        case SHARED_MODIFIED:
            ack(addr, procNum, srcProc, reqType, msgNum);
            if (reqType == DATA)
            {
                *ca = DATA_RECV;
                return MODIFIED;
            }
            return SHARED_MODIFIED;
        case INVALID_MODIFIED:
            ack(addr, procNum, srcProc, reqType, msgNum);
            if (reqType == DATA)
            {
                *ca = DATA_RECV;
                return MODIFIED;
            }

            return INVALID_MODIFIED;
        case INVALID_SHARED_EXCLUSIVE:
            ack(addr, procNum, srcProc, reqType, msgNum);
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