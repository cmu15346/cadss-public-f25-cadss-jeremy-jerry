#include <getopt.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <stdbool.h>
#include "processor.h"
#include "trace.h"
#include "cache.h"
#include "branch.h"

trace_reader* tr = NULL;
cache* cs = NULL;
branch* bs = NULL;
processor* self = NULL;

typedef struct _functionUnit FU;

typedef struct _functionalUnits {
    FU* fastALUs;
    FU* longALUs;
} scoreboard;

typedef struct _register {
    bool ready;
    int num;
    int64_t tag;
} reg;

typedef struct _registerFile {
    reg* regs;
} RF;

typedef struct _reservationStation {
    FU* FU;
    int64_t tag;
    reg** srcs;
    reg* dest;
    bool isLongALU;
    struct _reservationStation* next;
    struct _reservationStation* prev;
} RS;

typedef struct _commonDataBus {
    bool busy;
    int64_t tag;
    reg* dest;
} CDB;

int processorCount = 1;
int CADSS_VERBOSE = 0;

int* pendingMem = NULL;
int* pendingBranch = NULL;
int64_t* memOpTag = NULL;

int fetchRate = 0;
int dispatchWidth = 0;
int scheduleWidth = 0;
int numFastALU = 0;
int numLongALU = 0;
int numCDB = 0;
uint64_t tagCounter = 0;

typedef struct _DQEntry {
    trace_op* op;
    struct _DQEntry* next;
} DQEntry;

typedef struct _dispatchQueue {
    DQEntry* head;
    DQEntry* tail;
    int size;
    int maxSize;
} dispatchQueue;

typedef struct scheduleQueue {
    RS* head;
    int sizeFast;
    int sizeLong;
    int maxFastSize;
    int maxLongSize;
} scheduleQueue;

typedef struct _functionUnit {
    bool busy;
    bool isLongALU;
    RS* executingEntry1;
    RS* executingEntry2;
    RS* executingEntry3;
} FU;


dispatchQueue* DQ = NULL;
scheduleQueue* SQ = NULL;
scoreboard* sb = NULL;
RF* rf = NULL;
CDB* cdbs = NULL;

int64_t getNextTag() {
    int64_t tag = tagCounter;
    tagCounter++;
    return tag;
}

void initialize() {
    //initialize functional units
    sb = malloc(sizeof(scoreboard));
    sb->fastALUs = calloc(numFastALU, sizeof(FU));
    sb->longALUs = calloc(numLongALU, sizeof(FU));
    for (int i = 0; i < numFastALU; i++) {
        sb->fastALUs[i].busy = false;
        sb->fastALUs[i].isLongALU = false;
    }
    for (int i = 0; i < numLongALU; i++) {
        sb->longALUs[i].busy = false;
        sb->longALUs[i].isLongALU = true;
    }
    //initialize register file
    rf = malloc(sizeof(RF));
    rf->regs = malloc(33 * sizeof(reg));
    for (int i = 0; i < 33; i++) {
        rf->regs[i].ready = true;
        rf->regs[i].num = i;
        rf->regs[i].tag = -1;
    }
    //initialize dispatch queue
    DQ = malloc(sizeof(dispatchQueue));
    DQ->head = NULL;
    DQ->tail = NULL;
    DQ->size = 0;
    DQ->maxSize = dispatchWidth * (scheduleWidth*numFastALU + scheduleWidth*numLongALU);
    //initialize schedule queue
    SQ = malloc(sizeof(scheduleQueue));
    SQ->head = NULL;
    SQ->sizeFast = 0;
    SQ->sizeLong = 0;
    SQ->maxFastSize = scheduleWidth * numFastALU;
    SQ->maxLongSize = scheduleWidth * numLongALU;
    //initialize CDBs
    cdbs = malloc(numCDB * sizeof(CDB));
    for (int i = 0; i < numCDB; i++) {
        cdbs[i].busy = false;
        cdbs[i].tag = -1;
        cdbs[i].dest = NULL;
    }
}

//FU operations
FU* getFreeFU(bool isLongALU) {
    if (isLongALU) {
        for (int i = 0; i < numLongALU; i++) {
            if (!sb->longALUs[i].busy) {
                return &sb->longALUs[i];
            }
        }
    }
    else {
        for (int i = 0; i < numFastALU; i++) {
            if (!sb->fastALUs[i].busy) {
                return &sb->fastALUs[i];
            }
        }
    }
    return NULL;
}

//dispatch stage operations

bool addToDQ(trace_op* op) {
    if (DQ->size >= DQ->maxSize) {
        return false;
    }
    DQEntry* newEntry = malloc(sizeof(DQEntry));
    newEntry->op = op;
    newEntry->next = NULL;
    if (DQ->tail == NULL) {
        DQ->head = newEntry;
        DQ->tail = newEntry;
    }
    else {
        DQ->tail->next = newEntry;
        DQ->tail = newEntry;
    }
    DQ->size++;
    return true;
}

trace_op* removeFromDQ() {
    if (DQ->size == 0) {
        return NULL;
    }
    DQEntry* entry = DQ->head;
    trace_op* op = entry->op;
    DQ->head = entry->next;
    if (DQ->head == NULL) {
        DQ->tail = NULL;
    }
    free(entry);
    DQ->size--;
    return op;
}

trace_op* peekDQ() {
    if (DQ->size == 0) {
        return NULL;
    }
    return DQ->head->op;
}

//schedule queue operations
bool isFullSQ(bool isLongALU) {
    if (isLongALU) {
        return SQ->sizeLong >= SQ->maxLongSize;
    }
    else {
        return SQ->sizeFast >= SQ->maxFastSize;
    }
}

bool addToSQ(RS* newEntry, bool isLongALU) {
    if (isLongALU) {
        if (SQ->sizeLong >= SQ->maxLongSize) {
            return false;
        }
        newEntry->next = SQ->head;
        newEntry->prev = NULL;
        if (SQ->head != NULL) {
            SQ->head->prev = newEntry;
        }
        SQ->head = newEntry;
        SQ->sizeLong++;
        return true;
    }
    else {
        if (SQ->sizeFast >= SQ->maxFastSize) {
            return false;
        }
        newEntry->next = SQ->head;
        newEntry->prev = NULL;
        if (SQ->head != NULL) {
            SQ->head->prev = newEntry;
        }
        SQ->head = newEntry;
        SQ->sizeFast++;
        return true;
    }
}

void removeFromSQ(RS* entry){
    if (entry->isLongALU) {
        if (entry->prev != NULL) {
            entry->prev->next = entry->next;
        }
        else {
            SQ->head = entry->next;
        }
        if (entry->next != NULL) {
            entry->next->prev = entry->prev;
        }
        SQ->sizeLong--;
    }
    else {
        if (entry->prev != NULL) {
            entry->prev->next = entry->next;
        }
        else {
            SQ->head = entry->next;
        }
        if (entry->next != NULL) {
            entry->next->prev = entry->prev;
        }
        SQ->sizeFast--;
    }
    free(entry);
}

// Dispatch stage
void dispatch() {
    int dispatched = 0;
    while (dispatched < dispatchWidth) {
        trace_op* nextOp = peekDQ();
        if (nextOp == NULL) {
            break;
        }
        //check if schedule queue has room
        bool isLongALU = (nextOp->op == ALU_LONG);
        if (isFullSQ(isLongALU)) {
            break;
        }
        //remove from dispatch queue
        trace_op* op = removeFromDQ();
        if (op == NULL) {
            break;
        }
        RS* rs = malloc(sizeof(RS));
        rs->FU = NULL;
        rs->isLongALU = (op->op == ALU_LONG);
        rs->srcs = malloc(2 * sizeof(reg*));
        int reg1 = op->src_reg[0];
        int reg2 = op->src_reg[1];
        int regD = op->dest_reg;
        reg* src1;
        reg* src2;
        if (reg1 == -1) {
            src1 = NULL;
        } else {
            src1 = &rf->regs[reg1];
        }
        if (reg2 == -1) {
            src2 = NULL;
        } else {
            src2 = &rf->regs[reg2];
        }
        reg* dest;
        if (regD != -1) {
            dest = &rf->regs[op->dest_reg];
        }
        else {
            dest = NULL;
        }
        rs->dest = malloc(sizeof(reg));
        if (dest != NULL) {
            rs->dest->num = dest->num;
        }
        else {
            rs->dest->num = -1;
        }
        //check source readiness/update tags
        for (int i = 0; i < 2; i++) {
            reg* src = (i == 0) ? src1 : src2;
            if (src == NULL) {
                rs->srcs[i] = malloc(sizeof(reg));
                rs->srcs[i]->ready = true;
                continue;
            }
            rs->srcs[i] = malloc(sizeof(reg));
            rs->srcs[i]->num = src->num;
            if (src->ready) {
                rs->srcs[i]->ready = true;
            }
            else {
                rs->srcs[i]->ready = false;
                rs->srcs[i]->tag = src->tag;
            }
            int64_t tag = getNextTag();
            if (dest != NULL) {
                dest->tag = tag;
                dest->ready = false;
            }
            rs->dest->tag = tag;
            rs->dest->ready = false;
        }
        //add to schedule queue(we know it has room if we get here)
        addToSQ(rs, isLongALU);
        dispatched++;
    }
}

//schedule stage
void schedule() {
    int scheduled = 0;
    for (RS* rs = SQ->head; rs != NULL && scheduled < scheduleWidth; rs = rs->next) {
        //check if CDB broadcasted one of our dependencies
        //set all CDBs to be not busy after we cal this function
        if (rs->FU != NULL) {
            //printf("RS %p already has FU %p\n", rs, rs->FU);
            continue; //already scheduled
        }
        //printf("Scheduling RS %p\n", rs);
        for (int i = 0; i < numCDB; i++) {
            if (cdbs[i].busy) {
                for (int j = 0; j < 2; j++) {
                    if (!rs->srcs[j]->ready && rs->srcs[j]->tag == cdbs[i].tag) {
                        rs->srcs[j]->ready = true;
                    }
                }
            }
        }
        if (rs->srcs[0]->ready && rs->srcs[1]->ready) {
            //wake up, FIFO selection
            FU* fu = getFreeFU(rs->isLongALU);
            if (fu != NULL) {
                rs->FU = fu; 
                fu->busy = true;
                fu->executingEntry1 = rs;
                scheduled++;
            }
        }
    }
}

//execute stage
typedef struct _wrapper {
    RS* rs;
    struct _wrapper* next;
    struct _wrapper* prev;
} completedNode;

completedNode* completed = NULL;

bool completed_contains(RS* e) {
    for (completedNode* n = completed; n != NULL; n = n->next) {
        if (n->rs == e) return true;
    }
    return false;
}

void addToCompleted(RS* e) {
    //assert(!completed_contains(e));
    completedNode* entry = malloc(sizeof(completedNode));
    entry->rs = e;
    entry->next = NULL;
    entry->prev = NULL;
    if (completed == NULL) {
        completed = entry;
        entry->next = NULL;
        entry->prev = NULL;
    }
    else {
        entry->next = completed;
        entry->prev = NULL;
        completed->prev = entry;
        completed = entry;
    }
}

RS* removeByMinTag() {
    if (completed == NULL) {
        return NULL;
    }
    completedNode* minEntry = completed;
    for (completedNode* entry = completed; entry != NULL; entry = entry->next) {
        if (entry->rs->dest->tag < minEntry->rs->dest->tag) {
            minEntry = entry;
        }
    }
    //remove minEntry from completed list
    if (minEntry->prev != NULL) {
        minEntry->prev->next = minEntry->next;
    } else {
        completed = minEntry->next;
    }
    if (minEntry->next != NULL) {
        minEntry->next->prev = minEntry->prev;
    }
    RS* returnEntry = minEntry->rs;
    free(minEntry);
    return returnEntry;
}

void execute() {
    //for each FU, if busy, execute
    for (int i = 0; i < numFastALU; i++) {
        FU* fu = &sb->fastALUs[i];
        if (fu->busy && fu->executingEntry1 != NULL) {
            //not pipelined, so complete in one cycle
            fu->busy = false;
            addToCompleted(fu->executingEntry1);
            fu->executingEntry1 = NULL;
        }
    }
    for (int i = 0; i < numLongALU; i++) {
        FU* fu = &sb->longALUs[i];
        //pipelined FU, so we can have up to 3 entries executing
        if (fu->executingEntry3 != NULL) {
            addToCompleted(fu->executingEntry3);
            fu->executingEntry3 = NULL;
        }
        if (fu->executingEntry2 != NULL) {
            fu->executingEntry3 = fu->executingEntry2;
            fu->executingEntry2 = NULL;
        }
        if (fu->busy && fu->executingEntry1 != NULL) {
            fu->busy = false;
            fu->executingEntry2 = fu->executingEntry1;
            fu->executingEntry1 = NULL;
        }
    }
}

//state update stage
void stateUpdate() {
    for (int i = 0; i < numCDB; i++) {
        cdbs[i].busy = false;
        cdbs[i].tag = -1;
        cdbs[i].dest = NULL;
    }
    for (int i = 0; i < numCDB; i++) {
        RS* rs = removeByMinTag();
        if (rs == NULL) {
            break;
        }
        cdbs[i].busy = true;
        cdbs[i].tag = rs->dest->tag;
        cdbs[i].dest = rs->dest;
        if (rs->dest->num == -1) {
            //no destination register
            removeFromSQ(rs);
            continue;
        }
        reg* destReg = &rf->regs[rs->dest->num];
        if (destReg->tag == rs->dest->tag) {
            destReg->ready = true;
        }
        removeFromSQ(rs);
    }
}

//
// init
//
//   Parse arguments and initialize the processor simulator components
//
processor* init(processor_sim_args* psa)
{
    int op;

    tr = psa->tr;
    cs = psa->cache_sim;
    bs = psa->branch_sim;

    // TODO - get argument list from assignment
    while ((op = getopt(psa->arg_count, psa->arg_list, "f:d:m:j:k:c:")) != -1)
    {
        switch (op)
        {
            // fetch rate
            case 'f':
                fetchRate = atoi(optarg);
                break;

            // dispatch queue multiplier
            case 'd':
                dispatchWidth = atoi(optarg);
                break;

            // Schedule queue multiplier
            case 'm':
                scheduleWidth = atoi(optarg);
                break;

            // Number of fast ALUs
            case 'j':
                numFastALU = atoi(optarg);
                break;

            // Number of long ALUs
            case 'k':
                numLongALU = atoi(optarg);
                break;

            // Number of CDBs
            case 'c':
                numCDB = atoi(optarg);
                break;
        }
    }

    pendingBranch = calloc(processorCount, sizeof(int));
    pendingMem = calloc(processorCount, sizeof(int));
    memOpTag = calloc(processorCount, sizeof(int64_t));
    initialize();

    self = calloc(1, sizeof(processor));
    return self;
}

const int64_t STALL_TIME = 100000;
int64_t tickCount = 0;
int64_t stallCount = -1;

int64_t makeTag(int procNum, int64_t baseTag)
{
    return ((int64_t)procNum) | (baseTag << 8);
}

void memOpCallback(int procNum, int64_t tag)
{
    int64_t baseTag = (tag >> 8);

    // Is the completed memop one that is pending?
    if (baseTag == memOpTag[procNum])
    {
        memOpTag[procNum]++;
        pendingMem[procNum] = 0;
        stallCount = tickCount + STALL_TIME;
    }
    else
    {
        printf("memopTag: %ld != tag %ld\n", memOpTag[procNum], tag);
    }
}

int tick(void)
{
    // if room in pipeline, request op from trace
    //   for the sample processor, it requests an op
    //   each tick until it reaches a branch or memory op
    //   then it blocks on that op

    trace_op* nextOp = NULL;

    // Pass along to the branch predictor and cache simulator that time ticked
    bs->si.tick();
    cs->si.tick();
    tickCount++;

    if (tickCount == stallCount)
    {
        printf(
            "Processor may be stalled.  Now at tick - %ld, last op at %ld\n",
            tickCount, tickCount - STALL_TIME);
        for (int i = 0; i < processorCount; i++)
        {
            if (pendingMem[i] == 1)
            {
                printf("Processor %d is waiting on memory\n", i);
            }
        }
    }

    int progress = 0;
    for (int i = 0; i < processorCount; i++)
    {
        if (pendingMem[i] == 1)
        {
            progress = 1;
            continue;
        }

        // In the full processor simulator, the branch is pending until
        //   it has executed.
        if (pendingBranch[i] > 0)
        {
            pendingBranch[i]--;
            progress = 1;
            continue;
        }

        // TODO: get and manage ops for each processor core
        nextOp = tr->getNextOp(i);

        if (nextOp == NULL)
            continue;

        progress = 1;
        bool hasSpaceInDQ;
        for (int j = 0; j < fetchRate; j++) {
            switch (nextOp->op)
            {
                case MEM_LOAD:
                case MEM_STORE:
                    // pendingMem[i] = 1;
                    // cs->memoryRequest(nextOp, i, makeTag(i, memOpTag[i]),
                    //                 memOpCallback);
                    // break;

                case BRANCH:
                    // pendingBranch[i]
                    //     = (bs->branchRequest(nextOp, i) == nextOp->nextPCAddress)
                    //         ? 0
                    //         : 1;
                    // break;

                case ALU:
                case ALU_LONG:
                    hasSpaceInDQ = addToDQ(nextOp);
                    break;
            }
            nextOp = tr->getNextOp(i);
            if (nextOp == NULL || !hasSpaceInDQ) {
                break;
            }
        }
    }
    stateUpdate();
    execute();
    schedule();
    dispatch();

    return progress;
}

int finish(int outFd)
{
    int c = cs->si.finish(outFd);
    int b = bs->si.finish(outFd);

    char buf[32];
    size_t charCount = snprintf(buf, 32, "Ticks - %ld\n", tickCount);

    (void)!write(outFd, buf, charCount + 1);

    if (b || c)
        return 1;
    return 0;
}

int destroy(void)
{
    int c = cs->si.destroy();
    int b = bs->si.destroy();

    if (b || c)
        return 1;
    return 0;
}
