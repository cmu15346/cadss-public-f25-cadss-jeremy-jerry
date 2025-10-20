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
    int64_t tickIssued;
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
    RS* completedEntry;
} FU;


dispatchQueue* DQ = NULL;
scheduleQueue* SQ = NULL;
scoreboard* sb = NULL;
RF* rf = NULL;
CDB* cdbs = NULL;
CDB* cdbsIssued = NULL;
const int64_t STALL_TIME = 100000;
int64_t tickCount = 0;
int64_t stallCount = -1;

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
    cdbsIssued = malloc(numCDB * sizeof(CDB));
    for (int i = 0; i < numCDB; i++) {
        cdbs[i].busy = false;
        cdbs[i].tag = -1;
        cdbsIssued[i].busy = false;
        cdbsIssued[i].tag = -1;
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
bool isFullDQ() {
    return DQ->size >= DQ->maxSize;
}

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
int dispatch() {
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
        }
        int64_t tag = getNextTag();
        if (dest != NULL) {
            dest->tag = tag;
            dest->ready = false;
            rs->dest->tag = tag;
            rs->dest->ready = false;
        }
        else {
            rs->dest->tag = -1;
            rs->dest->ready = true;
        }
        //add to schedule queue(we know it has room if we get here)
        addToSQ(rs, isLongALU);
        dispatched++;
    }
    return dispatched;
}

RS* readyToFire[1024];

void addReadyToFire(RS* rs) {
    // if (!rs->isLongALU) {
    //     rs->FU->executingEntry1 = rs;
    //     return;
    // }
    for (int i = 0; i < 1024; i++) {
        if (readyToFire[i] == NULL) {
            readyToFire[i] = rs;
            return;
        }
    }
}

void fireReadyToFire() {
    for (int i = 0; i < 1024; i++) {
        if (readyToFire[i] != NULL) {
            RS* rs = readyToFire[i];
            FU* fu = rs->FU;
            fu->executingEntry1 = rs;
            readyToFire[i] = NULL;
        }
    }
}

//schedule stage
int schedule() {
    int scheduled = 0;
    for (RS* rs = SQ->head; rs != NULL; rs = rs->next) {
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
        if (rs->srcs[0]->ready && rs->srcs[1]->ready && scheduled < scheduleWidth) {
            //wake up, FIFO selection
            FU* fu = getFreeFU(rs->isLongALU);
            if (fu != NULL) {
                rs->FU = fu; 
                fu->busy = true;
                addReadyToFire(rs);
                scheduled++;
            }
        }
    }
    for (int i = 0; i < numCDB; i++) {
        cdbs[i].busy = false;
        cdbs[i].tag = -1;
    }
    return scheduled;
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

int execute() {
    //for each FU, if busy, execute
    int executed = 0;
    for (int i = 0; i < numFastALU; i++) {
        FU* fu = &sb->fastALUs[i];
        if (fu->busy && fu->executingEntry1 != NULL) {
            executed++;
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
            executed++;
            addToCompleted(fu->executingEntry3);
            fu->executingEntry3 = NULL;
        }
        if (fu->executingEntry2 != NULL) {
            executed++;
            fu->executingEntry3 = fu->executingEntry2;
            fu->executingEntry2 = NULL;
        }
        if (fu->busy && fu->executingEntry1 != NULL) {
            executed++;
            fu->busy = false;
            fu->executingEntry2 = fu->executingEntry1;
            fu->executingEntry1 = NULL;
        }
    }
    return executed;
}

RS* toRemoveFromSQ[1024];
int toRemoveCount = 0;
void addToRemoveFromSQ(RS* rs) {
    toRemoveFromSQ[toRemoveCount] = rs;
    toRemoveCount++;
}
void clearToRemoveFromSQ() {
    toRemoveCount = 0;
}
void removeAllFromSQ() {
    for (int i = 0; i < toRemoveCount; i++) {
        removeFromSQ(toRemoveFromSQ[i]);
    }
    clearToRemoveFromSQ();
}
//state update stage
int stateUpdate() {
    int updated = 0;
    for (int i = 0; i < numCDB; i++) {
        cdbsIssued[i].busy = false;
        cdbsIssued[i].tag = -1;
    }
    for (int i = 0; i < numCDB; i++) {
        RS* rs = removeByMinTag();
        if (rs == NULL) {
            break;
        }
        updated++;
        cdbsIssued[i].busy = true;
        cdbsIssued[i].tag = rs->dest->tag;
        if (rs->dest->num == -1) {
            //no destination register
            addToRemoveFromSQ(rs);
            continue;
        }
        reg* destReg = &rf->regs[rs->dest->num];
        if (destReg->tag == rs->dest->tag) {
            destReg->ready = true;
        }
        addToRemoveFromSQ(rs);
    }
    return updated;
}

void shiftCDBs()
{
    for (int i = 0; i < numCDB; i++) {
        cdbs[i].tag = cdbsIssued[i].tag;
        cdbs[i].busy = cdbsIssued[i].busy;
        cdbsIssued[i].busy = false;
        cdbsIssued[i].tag = -1;
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

int instructionCount = 0;
int tick(void)
{
    // if room in pipeline, request op from trace
    //   for the sample processor, it requests an op
    //   each tick until it reaches a branch or memory op
    //   then it blocks on that op

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

        bool hasSpaceInDQ;
        for (int j = 0; j < fetchRate; j++) {
            if (isFullDQ()) {
                break;
            }
            trace_op* nextOp = tr->getNextOp(i);
            if (nextOp == NULL) {
                break;
            }
            progress = 1;
            switch (nextOp->op)
            {
                case MEM_LOAD:
                case MEM_STORE:
                    pendingMem[i] = 1;
                    cs->memoryRequest(nextOp, i, makeTag(i, memOpTag[i]),
                                    memOpCallback);
                    break;

                case BRANCH:
                    pendingBranch[i]
                        = (bs->branchRequest(nextOp, i) == nextOp->nextPCAddress)
                            ? 0
                            : 1;
                    break;

                case ALU:
                case ALU_LONG:
                    addToDQ(nextOp);
                    break;
            }
        }
    }
    int executed = execute();
    int updated = stateUpdate();
    fireReadyToFire();
    int scheduled = schedule();
    int dispatched = dispatch();
    shiftCDBs();
    removeAllFromSQ();
    int inDQ = DQ->size;
    int inSQ = SQ->sizeFast + SQ->sizeLong;
    if (updated || executed || scheduled || dispatched || inDQ || inSQ) {
        progress = 1;
    }
    // if (tickCount % 10000 == 0) {
    //     printf("Tick %ld: dispatched %d, scheduled %d, executed %d, updated %d, DQ size %d, SQ size %d\n",
    //            tickCount, dispatched, scheduled, executed, updated, DQ->size,
    //            SQ->sizeFast + SQ->sizeLong);
    // }
    if (tickCount > 1000000) {
        printf("No progress after 1,000,000 ticks.  Exiting.\n");
        // Print contents of the schedule queue (SQ)
        printf("Schedule Queue: sizeFast=%d sizeLong=%d\n", SQ->sizeFast, SQ->sizeLong);
        RS* _sq_node = SQ->head;
        int _sq_idx = 0;
        while (_sq_node != NULL) {
            printf("SQ[%d] addr=%p isLongALU=%d FU=%p dest.num=%d dest.tag=%ld",
                   _sq_idx, (void*)_sq_node, _sq_node->isLongALU, (void*)_sq_node->FU,
                   _sq_node->dest ? _sq_node->dest->num : -1,
                   _sq_node->dest ? _sq_node->dest->tag : -1);
            for (int _s = 0; _s < 2; _s++) {
                if (_sq_node->srcs && _sq_node->srcs[_s]) {
                    printf(" src%d(ready=%d,num=%d,tag=%ld)",
                           _s,
                           _sq_node->srcs[_s]->ready ? 1 : 0,
                           _sq_node->srcs[_s]->num,
                           _sq_node->srcs[_s]->tag);
                } else {
                    printf(" src%d(NULL)", _s);
                }
            }
            printf("\n");
            _sq_node = _sq_node->next;
            _sq_idx++;
        }
        exit(1);
    }
    // printf("progress: %d\n", progress);
    // printf("schedule: %d, execute: %d, stateUpdate: %d, dispatch: %d\n",
    //        scheduled, executed, updated, dispatched);
    // printf("DQ size: %d, SQ fast size: %d, SQ long size: %d\n",
    //        DQ->size, SQ->sizeFast, SQ->sizeLong);
    // printf("isFullDQ: %d\n", isFullDQ());
    // printf("isFullSQ fast: %d, isFullSQ long: %d\n",
    //        isFullSQ(false), isFullSQ(true));
    // printf("CDBs: ");
    // for (int i = 0; i < numCDB; i++) {
    //     if (cdbs[i].busy) {
    //         printf("[tag: %ld] ", cdbs[i].tag);
    //     } else {
    //         printf("[idle] ");
    //     }
    // }
    // printf("\n");
    return progress;
}

int finish(int outFd)
{
    //printf("Total Instructions Executed: %d\n", instructionCount);
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
