/*
 * dbp.cpp — Dead Block Prediction (DBP) Cache Replacement Policy
 *
 * Intuition: a cache line is "dead" the moment its final access has happened.
 * If we can predict deadness at insertion time, we can put those lines at the
 * LRU position so they get evicted first — before lines likely to be reused.
 *
 * Mechanism:
 *   Prediction table:  PC → 2-bit saturating counter (0..3)
 *     0–1 = predicted dead   → insert at LRU (front of order, evicted first)
 *     2–3 = predicted live   → insert at MRU (back  of order, evicted last)
 *
 *   On eviction: update the counter for the line's last-touching PC.
 *     Line was reused   → saturating increment (toward live)
 *     Line never reused → saturating decrement (toward dead)
 *
 *   On hit: always move line to MRU regardless of original prediction.
 *
 * Knobs:
 *   -c  cache size in bytes            (default 8192)
 *   -a  associativity                  (default 2)
 *   -b  block size in bytes            (default 64)
 *   -d  default counter for new PCs    (default 2; range 0–3)
 *         0 or 1 → pessimistic (treat all unseen PCs as dead)
 *         2 or 3 → optimistic  (treat all unseen PCs as live)
 */

#include <stdio.h>
#include <stdint.h>
#include <vector>
#include <deque>
#include <algorithm>
#include <unordered_map>
#include <string>
#include "pin.H"

// ─── Knobs ────────────────────────────────────────────────────────────────────
KNOB<int> KnobCacheSize  (KNOB_MODE_WRITEONCE, "pintool", "c", "8192", "Cache size in bytes");
KNOB<int> KnobAssoc      (KNOB_MODE_WRITEONCE, "pintool", "a", "2",    "Associativity");
KNOB<int> KnobBlockSize  (KNOB_MODE_WRITEONCE, "pintool", "b", "64",   "Block size in bytes");
KNOB<int> KnobDefaultPred(KNOB_MODE_WRITEONCE, "pintool", "d", "2",
    "Default 2-bit counter for PCs seen for first time (0-3; <=1=dead >=2=live)");
KNOB<std::string> KnobOutputPath(
    KNOB_MODE_WRITEONCE,
    "pintool",
    "o",
    "traces/week4-dbp.log",
    "Output log path");

int CACHE_SIZE, ASSOCIATIVITY, BLOCK_SIZE, NUM_SETS;
int DEFAULT_PRED;   // 0–3
FILE* output = nullptr;

// ─── Prediction table ─────────────────────────────────────────────────────────
// Maps PC → 2-bit saturating counter. Unseen PCs return DEFAULT_PRED.
static std::unordered_map<ADDRINT, uint8_t> pred_table;

static inline uint8_t get_counter(ADDRINT pc)
{
    auto it = pred_table.find(pc);
    return (it == pred_table.end()) ? (uint8_t)DEFAULT_PRED : it->second;
}

static inline bool is_dead_pc(ADDRINT pc) { return get_counter(pc) <= 1; }

static void update_counter(ADDRINT pc, bool was_reused)
{
    uint8_t c = get_counter(pc);
    pred_table[pc] = was_reused
        ? (c < 3 ? c + 1 : 3)    // saturating increment → live
        : (c > 0 ? c - 1 : 0);   // saturating decrement → dead
}

// ─── Cache line ───────────────────────────────────────────────────────────────
struct CacheLine {
    ADDRINT tag;
    BOOL    valid;
    ADDRINT last_pc;          // PC of the instruction that last touched this line
    BOOL    was_reused;       // received at least one hit after insertion
    BOOL    inserted_as_dead; // prediction recorded at insertion time (for stats)
};

std::vector<std::vector<CacheLine>> cache;

// LRU order per set: stores way indices.
//   front = LRU victim (evicted first)
//   back  = MRU        (evicted last)
// Dead-predicted insertions → push_front (lowest priority).
// Live-predicted insertions → push_back  (highest priority).
std::vector<std::deque<int>> lru_order;

// ─── Stats ────────────────────────────────────────────────────────────────────
UINT64 hits               = 0;
UINT64 misses             = 0;
UINT64 dead_insertions    = 0;   // inserted with dead prediction
UINT64 live_insertions    = 0;   // inserted with live prediction
UINT64 correct_dead       = 0;   // predicted dead  → never reused ✓
UINT64 correct_live       = 0;   // predicted live  → was   reused ✓
UINT64 wrong_dead         = 0;   // predicted dead  → was   reused ✗
UINT64 wrong_live         = 0;   // predicted live  → never reused ✗

// ─── Helpers ──────────────────────────────────────────────────────────────────

static void lru_erase(std::deque<int> &dq, int way)
{
    auto it = std::find(dq.begin(), dq.end(), way);
    if (it != dq.end()) dq.erase(it);
}

// Called just before a way is overwritten. Updates prediction table and stats.
static void on_evict(int si, int way)
{
    auto &line = cache[si][way];
    if (!line.valid) return;

    bool reused = (bool)line.was_reused;
    update_counter(line.last_pc, reused);

    // Outcome vs prediction.
    if (line.inserted_as_dead) {
        reused ? wrong_dead++ : correct_dead++;
    } else {
        reused ? correct_live++ : wrong_live++;
    }
}

// Evict the LRU victim (front of order deque).
static int pick_victim(int si)
{
    // Prefer invalid (free) ways first — no eviction cost.
    for (int w = 0; w < ASSOCIATIVITY; w++)
        if (!cache[si][w].valid) return w;

    // All ways occupied: take LRU front.
    int victim = lru_order[si].front();
    lru_order[si].pop_front();
    on_evict(si, victim);
    return victim;
}

// ─── Main access handler ──────────────────────────────────────────────────────
// Receives both the memory address and the PC of the issuing instruction.
VOID AccessMemory(VOID *addr, VOID *iptr)
{
    ADDRINT block = (ADDRINT)addr  / (ADDRINT)BLOCK_SIZE;
    int     si    = (int)(block    % (ADDRINT)NUM_SETS);
    ADDRINT tag   = block          / (ADDRINT)NUM_SETS;
    ADDRINT pc    = (ADDRINT)iptr;

    auto &s   = cache[si];
    auto &ord = lru_order[si];

    // ── Hit check ────────────────────────────────────────────────────────────
    for (int w = 0; w < ASSOCIATIVITY; w++) {
        if (s[w].valid && s[w].tag == tag) {
            hits++;
            s[w].was_reused = TRUE;
            s[w].last_pc    = pc;
            // Promote to MRU regardless of original dead/live prediction.
            lru_erase(ord, w);
            ord.push_back(w);
            return;
        }
    }

    // ── Miss ─────────────────────────────────────────────────────────────────
    misses++;

    bool dead = is_dead_pc(pc);
    int  victim = pick_victim(si);

    s[victim].tag              = tag;
    s[victim].valid            = TRUE;
    s[victim].last_pc          = pc;
    s[victim].was_reused       = FALSE;
    s[victim].inserted_as_dead = (BOOL)dead;

    if (dead) {
        // Lowest priority: insert at LRU front → evicted before any live line.
        ord.push_front(victim);
        dead_insertions++;
    } else {
        // Normal MRU insertion.
        ord.push_back(victim);
        live_insertions++;
    }
}

// ─── PIN instrumentation ──────────────────────────────────────────────────────
// Note: IARG_INST_PTR passes the PC of the load/store instruction.
VOID Instruction(INS ins, VOID *v)
{
    UINT32 memOperands = INS_MemoryOperandCount(ins);
    for (UINT32 memOp = 0; memOp < memOperands; memOp++) {
        if (INS_MemoryOperandIsRead(ins, memOp) ||
            INS_MemoryOperandIsWritten(ins, memOp))
        {
            INS_InsertPredicatedCall(
                ins, IPOINT_BEFORE,
                (AFUNPTR)AccessMemory,
                IARG_MEMORYOP_EA, memOp,   // memory address
                IARG_INST_PTR,             // PC of this instruction
                IARG_END);
        }
    }
}

VOID Fini(INT32 code, VOID *v)
{
    UINT64 total      = hits + misses;
    UINT64 insertions = dead_insertions + live_insertions;
    UINT64 classified = correct_dead + correct_live + wrong_dead + wrong_live;

    fprintf(output, "DBP (default counter=%d → %s)\n",
            DEFAULT_PRED, DEFAULT_PRED <= 1 ? "pessimistic/dead" : "optimistic/live");
    fprintf(output, "Hits:                     %lu\n", hits);
    fprintf(output, "Misses:                   %lu\n", misses);
    if (total)
        fprintf(output, "HitRate:                  %.2f%%\n", (100.0 * hits) / total);

    fprintf(output, "---\n");
    fprintf(output, "Dead insertions:          %lu  (%.1f%% of misses)\n",
            dead_insertions,
            insertions ? (100.0 * dead_insertions) / insertions : 0.0);
    fprintf(output, "Live insertions:          %lu  (%.1f%% of misses)\n",
            live_insertions,
            insertions ? (100.0 * live_insertions) / insertions : 0.0);

    fprintf(output, "---\n");
    if (classified) {
        double pred_accuracy = (100.0 * (correct_dead + correct_live)) / classified;
        fprintf(output, "Prediction accuracy:      %.2f%%\n", pred_accuracy);
        fprintf(output, "  Correct dead (TP):      %lu\n",  correct_dead);
        fprintf(output, "  Correct live (TN):      %lu\n",  correct_live);
        fprintf(output, "  Wrong dead  (FP):       %lu  (evicted early but was reused)\n", wrong_dead);
        fprintf(output, "  Wrong live  (FN):       %lu  (kept too long, never reused)\n",  wrong_live);
    }
    fprintf(output, "Unique PCs in pred table: %zu\n", pred_table.size());
    fflush(output);
    fclose(output);

    printf("DBP (default counter=%d → %s)\n",
           DEFAULT_PRED, DEFAULT_PRED <= 1 ? "pessimistic/dead" : "optimistic/live");
    printf("Hits:                     %lu\n", hits);
    printf("Misses:                   %lu\n", misses);
    if (total)
        printf("HitRate:                  %.2f%%\n", (100.0 * hits) / total);

    printf("---\n");
    printf("Dead insertions:          %lu  (%.1f%% of misses)\n",
           dead_insertions,
           insertions ? (100.0 * dead_insertions) / insertions : 0.0);
    printf("Live insertions:          %lu  (%.1f%% of misses)\n",
           live_insertions,
           insertions ? (100.0 * live_insertions) / insertions : 0.0);

    printf("---\n");
    if (classified) {
        double pred_accuracy = (100.0 * (correct_dead + correct_live)) / classified;
        printf("Prediction accuracy:      %.2f%%\n", pred_accuracy);
        printf("  Correct dead (TP):      %lu\n",  correct_dead);
        printf("  Correct live (TN):      %lu\n",  correct_live);
        printf("  Wrong dead  (FP):       %lu  (evicted early but was reused)\n", wrong_dead);
        printf("  Wrong live  (FN):       %lu  (kept too long, never reused)\n",  wrong_live);
    }
    printf("Unique PCs in pred table: %zu\n", pred_table.size());
}

int main(int argc, char *argv[])
{
    if (PIN_Init(argc, argv)) return -1;

    std::string output_path = KnobOutputPath.Value();
    output = fopen(output_path.c_str(), "w");
    if (!output) {
        perror(output_path.c_str());
        return -1;
    }

    CACHE_SIZE    = KnobCacheSize.Value();
    ASSOCIATIVITY = KnobAssoc.Value();
    BLOCK_SIZE    = KnobBlockSize.Value();
    NUM_SETS      = CACHE_SIZE / (BLOCK_SIZE * ASSOCIATIVITY);
    DEFAULT_PRED  = KnobDefaultPred.Value();

    if (DEFAULT_PRED < 0 || DEFAULT_PRED > 3) {
        fprintf(stderr, "Error: -d must be 0, 1, 2, or 3\n");
        return 1;
    }

    printf("[DBP init] sets=%d, assoc=%d, default_pred=%d (%s)\n",
           NUM_SETS, ASSOCIATIVITY, DEFAULT_PRED,
           DEFAULT_PRED <= 1 ? "dead" : "live");

    CacheLine empty = {0, FALSE, 0, FALSE, FALSE};
    cache.resize(NUM_SETS, std::vector<CacheLine>(ASSOCIATIVITY, empty));
    lru_order.resize(NUM_SETS);

    INS_AddInstrumentFunction(Instruction, 0);
    PIN_AddFiniFunction(Fini, 0);
    PIN_StartProgram();
}