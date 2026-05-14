#include <stdio.h>
#include <vector>
#include <cstdlib>
#include <string>
#include "pin.H"

KNOB<int> KnobCacheSize (KNOB_MODE_WRITEONCE, "pintool", "c", "8192", "Cache size in bytes");
KNOB<int> KnobAssoc     (KNOB_MODE_WRITEONCE, "pintool", "a", "2",    "Associativity");
KNOB<int> KnobBlockSize (KNOB_MODE_WRITEONCE, "pintool", "b", "64",   "Block size in bytes");
KNOB<std::string> KnobOutputPath(
    KNOB_MODE_WRITEONCE,
    "pintool",
    "o",
    "traces/week4-rrip-sp.log",
    "Output log path");

int CACHE_SIZE, ASSOCIATIVITY, BLOCK_SIZE, NUM_SETS;
FILE* output = nullptr;

#define RRPV_HIT      0
#define RRPV_DEMAND   2
#define RRPV_PREFETCH 3
#define RRPV_MAX      3

struct CacheLine {
    ADDRINT tag;
    BOOL    valid;
    int     rrpv; // 0 - 3
    BOOL    prefetched;
};

std::vector<std::vector<CacheLine>> cache;

//  confidence: 0 = init, 1 = transient, 2 = steady 
struct StridePredictor {
    ADDRINT last_addr;
    ADDRINT last_stride;
    int     confidence;
    BOOL    initialized;
} sp = {0, 0, 0, FALSE};

UINT64 hits              = 0;
UINT64 misses            = 0;
UINT64 prefetch_attempts = 0;
UINT64 useful_prefetches = 0; 
UINT64 useless_prefetches= 0;
UINT64 prefetch_skipped  = 0;
UINT64 demand_hits_from_prefetch = 0;


static inline int find_way(int set_idx, ADDRINT tag)
{
    auto &s = cache[set_idx];
    for (int w = 0; w < ASSOCIATIVITY; w++)
        if (s[w].valid && s[w].tag == tag) return w;
    return -1;
}

static int rrip_victim(int set_idx)
{
    auto &s = cache[set_idx];
    while (true) {
        for (int w = 0; w < ASSOCIATIVITY; w++)
            if (!s[w].valid)        return w;
        for (int w = 0; w < ASSOCIATIVITY; w++)
            if (s[w].rrpv == RRPV_MAX) return w;
        // Age all lines by 1
        for (int w = 0; w < ASSOCIATIVITY; w++)
            s[w].rrpv++;
    }
}

// Find a way suitable for a prefetch without evicting hot lines.
static int prefetch_victim(int set_idx)
{
    auto &s = cache[set_idx];
    for (int w = 0; w < ASSOCIATIVITY; w++)
        if (!s[w].valid) return w;
    for (int w = 0; w < ASSOCIATIVITY; w++)
        if (s[w].rrpv == RRPV_MAX) return w;
    return -1;
}

static void update_stride(ADDRINT addr)
{
    if (!sp.initialized) {
        sp.last_addr   = addr;
        sp.confidence  = 0;
        sp.initialized = TRUE;
        return;
    }
    ADDRINT new_stride = addr - sp.last_addr;
    if (new_stride == sp.last_stride) {
        if (sp.confidence < 2) sp.confidence++;
    } else {
        sp.confidence  = 0;
        sp.last_stride = new_stride;
    }
    sp.last_addr = addr;
}

static void try_prefetch()
{
    if (sp.confidence < 2) return;

    ADDRINT pf_addr    = sp.last_addr + sp.last_stride;
    ADDRINT pf_block   = pf_addr / BLOCK_SIZE;
    ADDRINT pf_set_idx = pf_block % NUM_SETS;
    ADDRINT pf_tag     = pf_block / NUM_SETS;

    prefetch_attempts++;

    if (find_way((int)pf_set_idx, pf_tag) != -1) return;

    int w = prefetch_victim((int)pf_set_idx);
    if (w == -1) { prefetch_skipped++; return; }

    auto &line = cache[pf_set_idx][w];

    if (line.valid && line.prefetched)
        useless_prefetches++;

    line.tag        = pf_tag;
    line.valid      = TRUE;
    line.rrpv       = RRPV_PREFETCH;
    line.prefetched = TRUE;
}

VOID AccessMemory(VOID* addr)
{
    ADDRINT address   = (ADDRINT)addr;
    ADDRINT blockAddr = address / BLOCK_SIZE;
    int     set_idx   = (int)(blockAddr % NUM_SETS);
    ADDRINT tag       = blockAddr / NUM_SETS;

    int way = find_way(set_idx, tag);

    if (way != -1) {
        hits++;
        auto &line = cache[set_idx][way];
        if (line.prefetched) {
            useful_prefetches++;
            demand_hits_from_prefetch++;
            line.prefetched = FALSE; 
        }
        line.rrpv = RRPV_HIT; 

        update_stride(address);
        try_prefetch();

    } else {
        misses++;

        int victim = rrip_victim(set_idx);
        auto &line = cache[set_idx][victim];

        if (line.valid && line.prefetched)
            useless_prefetches++;

        line.tag        = tag;
        line.valid      = TRUE;
        line.rrpv       = RRPV_DEMAND;
        line.prefetched = FALSE;

        update_stride(address);
        try_prefetch();
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
    UINT64 total = hits + misses;
    fprintf(output, "RRIP-SP\n");
    fprintf(output, "Hits:                    %lu\n", hits);
    fprintf(output, "Misses:                  %lu\n", misses);
    if (total)
        fprintf(output, "HitRate:                 %.2f%%\n", (100.0 * hits) / total);

    fprintf(output, "---\n");
    fprintf(output, "Prefetch attempts:       %lu\n", prefetch_attempts);
    if (prefetch_attempts) {
        fprintf(output, "Useful prefetches:       %lu  (%.1f%%)\n",
                useful_prefetches,
                (100.0 * useful_prefetches) / prefetch_attempts);
        fprintf(output, "Useless prefetches:      %lu  (%.1f%%)\n",
                useless_prefetches,
                (100.0 * useless_prefetches) / prefetch_attempts);
    }
    fprintf(output, "Prefetches skipped:      %lu\n", prefetch_skipped);
    if (hits)
        fprintf(output, "Hits from prefetch:      %lu  (%.1f%% of all hits)\n",
                demand_hits_from_prefetch,
                (100.0 * demand_hits_from_prefetch) / hits);
    fflush(output);
    fclose(output);

    printf("RRIP-SP\n");
    printf("Hits:                    %lu\n", hits);
    printf("Misses:                  %lu\n", misses);
    if (total)
        printf("HitRate:                 %.2f%%\n", (100.0 * hits) / total);

    printf("---\n");
    printf("Prefetch attempts:       %lu\n", prefetch_attempts);
    if (prefetch_attempts) {
        printf("Useful prefetches:       %lu  (%.1f%%)\n",
               useful_prefetches,
               (100.0 * useful_prefetches) / prefetch_attempts);
        printf("Useless prefetches:      %lu  (%.1f%%)\n",
               useless_prefetches,
               (100.0 * useless_prefetches) / prefetch_attempts);
    }
    printf("Prefetches skipped:      %lu\n", prefetch_skipped);
    if (hits)
        printf("Hits from prefetch:      %lu  (%.1f%% of all hits)\n",
               demand_hits_from_prefetch,
               (100.0 * demand_hits_from_prefetch) / hits);
}

int main(int argc, char* argv[])
{
    if (PIN_Init(argc, argv)) return -1;

    std::string output_path = KnobOutputPath.Value();
    output = fopen(output_path.c_str(), "w");
    if (!output)
    {
        perror(output_path.c_str());
        return -1;
    }

    CACHE_SIZE    = KnobCacheSize.Value();
    ASSOCIATIVITY = KnobAssoc.Value();
    BLOCK_SIZE    = KnobBlockSize.Value();
    NUM_SETS      = CACHE_SIZE / (BLOCK_SIZE * ASSOCIATIVITY);

    CacheLine empty = {0, FALSE, RRPV_MAX, FALSE};
    cache.resize(NUM_SETS, std::vector<CacheLine>(ASSOCIATIVITY, empty));

    INS_AddInstrumentFunction(Instruction, 0);
    PIN_AddFiniFunction(Fini, 0);
    PIN_StartProgram();
}