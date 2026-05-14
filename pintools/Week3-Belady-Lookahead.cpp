#include <stdio.h>
#include <vector>
#include <deque>
#include "pin.H"

// params
KNOB<int> KnobCacheSize  (KNOB_MODE_WRITEONCE, "pintool", "c", "8192", "Cache size in bytes");
KNOB<int> KnobAssoc      (KNOB_MODE_WRITEONCE, "pintool", "a", "2",    "Associativity");
KNOB<int> KnobBlockSize  (KNOB_MODE_WRITEONCE, "pintool", "b", "64",   "Block size in bytes");
KNOB<int> KnobWindowSize (KNOB_MODE_WRITEONCE, "pintool", "w", "1000", "Lookahead window size W");

int CACHE_SIZE, ASSOCIATIVITY, BLOCK_SIZE, NUM_SETS, WINDOW_SIZE;
static FILE* output = nullptr;
static const char* kOutputPath = "traces/week3-belady-lookahead.log";

// cache data structure
struct CacheLine {
    ADDRINT tag;
    BOOL    valid;
};
std::vector<std::vector<CacheLine>> cache;

// stores [0 to size-1] future window, holds block addresses
std::deque<ADDRINT> lookahead;

UINT64 hits   = 0;
UINT64 misses = 0;


// get block address
static inline ADDRINT block_of(int set_idx, int way)
{
    return cache[set_idx][way].tag * (ADDRINT)NUM_SETS + (ADDRINT)set_idx;
}

// search lookahead for our current access and otherwise return theoretical infinity (WINDOW_SIZE + 1)
static int next_use_distance(ADDRINT block_addr)
{
    int sz = (int)lookahead.size();
    for (int i = 0; i < sz; i++)
        if (lookahead[i] == block_addr) return i + 1;
    return WINDOW_SIZE + 1;
}

// select what to remove
static int belady_victim(int set_idx)
{
    // compulsory miss, no eviction
    for (int w = 0; w < ASSOCIATIVITY; w++)
        if (!cache[set_idx][w].valid) return w;

    int victim   = 0;
    int max_dist = -1;

    for (int w = 0; w < ASSOCIATIVITY; w++) {
        int dist = next_use_distance(block_of(set_idx, w));
        if (dist > max_dist) {
            max_dist = dist;
            victim   = w;

            if (dist == WINDOW_SIZE + 1) break;
        }
    }
    return victim;
}

// call belady for each access
static void process_access(ADDRINT block_addr)
{
    int set_idx = (int)(block_addr % (ADDRINT)NUM_SETS);
    ADDRINT tag = block_addr / (ADDRINT)NUM_SETS;

    auto &s = cache[set_idx];

    // hit
    for (int w = 0; w < ASSOCIATIVITY; w++) {
        if (s[w].valid && s[w].tag == tag) {
            hits++;
            return;
        }
    }

    // miss
    misses++;
    int victim = belady_victim(set_idx);
    s[victim].tag   = tag;
    s[victim].valid = TRUE;
}

VOID AccessMemory(VOID* addr)
{
    ADDRINT block_addr = (ADDRINT)addr / (ADDRINT)BLOCK_SIZE;
    lookahead.push_back(block_addr);

    if ((int)lookahead.size() > WINDOW_SIZE) {
        ADDRINT current = lookahead.front();
        lookahead.pop_front();
        // load lookahead with W  accesses after `current`
        process_access(current);
    }
}

VOID Instruction(INS ins, VOID* v)
{
    UINT32 memOperands = INS_MemoryOperandCount(ins);
    for (UINT32 memOp = 0; memOp < memOperands; memOp++) {
        if (INS_MemoryOperandIsRead(ins, memOp) ||
            INS_MemoryOperandIsWritten(ins, memOp))
        {
            INS_InsertPredicatedCall(
                ins, IPOINT_BEFORE,
                (AFUNPTR)AccessMemory,
                IARG_MEMORYOP_EA, memOp,
                IARG_END);
        }
    }
}

VOID Fini(INT32 code, VOID* v)
{

    // caveat - belady simulator has less context window towards the end but this is ok as no more cache acccess will occur thereafter
    while (!lookahead.empty()) {
        ADDRINT current = lookahead.front();
        lookahead.pop_front();
        process_access(current);
    }

    UINT64 total = hits + misses;
    fprintf(output, "Belady (lookahead W=%d)\n", WINDOW_SIZE);
    fprintf(output, "Hits:    %lu\n", hits);
    fprintf(output, "Misses:  %lu\n", misses);
    if (total)
        fprintf(output, "HitRate: %.2f%%\n", (100.0 * hits) / total);
    fflush(output);
    fclose(output);

    printf("Belady (lookahead W=%d)\n", WINDOW_SIZE);
    printf("Hits:    %lu\n", hits);
    printf("Misses:  %lu\n", misses);
    if (total)
        printf("HitRate: %.2f%%\n", (100.0 * hits) / total);
}

int main(int argc, char* argv[])
{
    if (PIN_Init(argc, argv)) return -1;

    output = fopen(kOutputPath, "w");
    if (!output) {
        perror(kOutputPath);
        return -1;
    }

    CACHE_SIZE    = KnobCacheSize.Value();
    ASSOCIATIVITY = KnobAssoc.Value();
    BLOCK_SIZE    = KnobBlockSize.Value();
    NUM_SETS      = CACHE_SIZE / (BLOCK_SIZE * ASSOCIATIVITY);
    WINDOW_SIZE   = KnobWindowSize.Value();

    CacheLine empty = {0, FALSE};
    cache.resize(NUM_SETS, std::vector<CacheLine>(ASSOCIATIVITY, empty));

    INS_AddInstrumentFunction(Instruction, 0);
    PIN_AddFiniFunction(Fini, 0);
    PIN_StartProgram();
}