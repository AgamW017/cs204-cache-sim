/*
  Week 1: Memory Trace + Pattern Analysis
  1. Reuse Distance (Temporal Locality)
  2. Stride (Spatial Locality)
 */

#include <stdio.h>
#include <list>
#include <vector>
#include <algorithm>
#include <unordered_map>
#include "pin.H"

FILE* trace;
const char* kTracePath = "traces/week1-trace.csv";

// LRU stack for reuse distance
std::list<ADDRINT> stackList;

// For stride
ADDRINT lastAddr = 0;
bool hasLast = false;

// Summary stats for practical inference.
UINT64 totalAccesses = 0;
UINT64 firstTouches = 0;
UINT64 reusedTouches = 0;
UINT64 reuseDistLE4 = 0;
UINT64 reuseDist5To32 = 0;
UINT64 reuseDistGT32 = 0;
UINT64 reuseDistSum = 0;

UINT64 strideSamples = 0;
UINT64 strideZero = 0;
UINT64 stridePositive = 0;
UINT64 strideNegative = 0;
UINT64 absStrideLE8 = 0;
UINT64 absStride9To64 = 0;
UINT64 absStride65To512 = 0;
UINT64 absStrideGT512 = 0;
UINT64 absStrideSum = 0;
UINT64 boundedStrideSamples = 0;
UINT64 boundedAbsStrideSum = 0;

const UINT64 kStrideBound = 4096;

// Keep a bounded stride histogram so we can report top repeating strides.
std::unordered_map<long long, UINT64> strideFreq;

VOID RecordAccess(VOID* addr)
{
    ADDRINT address = (ADDRINT)addr;
    totalAccesses++;

    int distance = 0;
    bool found = false;

    auto it = stackList.begin();
    for (; it != stackList.end(); it++, distance++)
    {
        if (*it == address)
        {
            found = true;
            break;
        }
    }

    if (found)
        stackList.erase(it);

    stackList.push_front(address);

    if (found)
    {
        reusedTouches++;
        reuseDistSum += (UINT64)distance;

        if (distance <= 4)
            reuseDistLE4++;
        else if (distance <= 32)
            reuseDist5To32++;
        else
            reuseDistGT32++;
    }
    else
    {
        firstTouches++;
    }

    // stride
    long stride = 0;
    if (hasLast)
    {
        stride = (long)(address - lastAddr);

        strideSamples++;
        if (stride == 0)
            strideZero++;
        else if (stride > 0)
            stridePositive++;
        else
            strideNegative++;

        UINT64 absStride = (stride >= 0) ? (UINT64)stride : (UINT64)(-stride);
        absStrideSum += absStride;

        if (absStride <= 8)
            absStrideLE8++;
        else if (absStride <= 64)
            absStride9To64++;
        else if (absStride <= 512)
            absStride65To512++;
        else
            absStrideGT512++;

        if (absStride <= kStrideBound)
        {
            boundedStrideSamples++;
            boundedAbsStrideSum += absStride;
            strideFreq[(long long)stride]++;
        }
    }

    // out
    // address, reuse_distance (-1 if first), stride
    if (found)
        fprintf(trace, "%lx,%d,%ld\n", address, distance, stride);
    else
        fprintf(trace, "%lx,-1,%ld\n", address, stride);

    lastAddr = address;
    hasLast = true;
}

VOID Instruction(INS ins, VOID* v)
{
    UINT32 memOperands = INS_MemoryOperandCount(ins);

    for (UINT32 memOp = 0; memOp < memOperands; memOp++)
    {
        if (INS_MemoryOperandIsRead(ins, memOp) ||
            INS_MemoryOperandIsWritten(ins, memOp))
        {
            INS_InsertPredicatedCall(
                ins, IPOINT_BEFORE,
                (AFUNPTR)RecordAccess,
                IARG_MEMORYOP_EA, memOp,
                IARG_END);
        }
    }
}

VOID Fini(INT32 code, VOID* v)
{
    fclose(trace);

    double reuseRate = (totalAccesses > 0)
        ? (100.0 * (double)reusedTouches / (double)totalAccesses) : 0.0;
    double avgReuseDist = (reusedTouches > 0)
        ? ((double)reuseDistSum / (double)reusedTouches) : 0.0;
    double avgAbsStride = (strideSamples > 0)
        ? ((double)absStrideSum / (double)strideSamples) : 0.0;
    double avgAbsStrideBounded = (boundedStrideSamples > 0)
        ? ((double)boundedAbsStrideSum / (double)boundedStrideSamples) : 0.0;

    std::vector<std::pair<long long, UINT64>> topStrides;
    topStrides.reserve(strideFreq.size());
    for (auto &kv : strideFreq)
        topStrides.push_back(kv);

    std::sort(topStrides.begin(), topStrides.end(),
        [](const std::pair<long long, UINT64>& a, const std::pair<long long, UINT64>& b)
        {
            return a.second > b.second;
        });

    printf("\n==== Week1 Inference Summary ====\n");
    printf("Total accesses: %llu\n", (unsigned long long)totalAccesses);
    printf("First touches: %llu\n", (unsigned long long)firstTouches);
    printf("Reused touches: %llu (%.2f%%)\n",
        (unsigned long long)reusedTouches, reuseRate);
    printf("Avg reuse distance (reused only): %.2f\n", avgReuseDist);
    printf("Reuse distance buckets: <=4=%llu, 5..32=%llu, >32=%llu\n",
        (unsigned long long)reuseDistLE4,
        (unsigned long long)reuseDist5To32,
        (unsigned long long)reuseDistGT32);

    printf("Stride samples: %llu\n", (unsigned long long)strideSamples);
    printf("Avg |stride| (all): %.2f bytes\n", avgAbsStride);
    printf("Avg |stride| (<=%llu): %.2f bytes\n",
        (unsigned long long)kStrideBound, avgAbsStrideBounded);
    printf("Stride sign: zero=%llu, pos=%llu, neg=%llu\n",
        (unsigned long long)strideZero,
        (unsigned long long)stridePositive,
        (unsigned long long)strideNegative);

    printf("Top repeating strides (|stride|<=4096): ");
    if (topStrides.empty())
    {
        printf("none\n");
    }
    else
    {
        UINT32 limit = (topStrides.size() < 5) ? (UINT32)topStrides.size() : 5;
        for (UINT32 i = 0; i < limit; i++)
        {
            printf("%lld(%llu)%s",
                topStrides[i].first,
                (unsigned long long)topStrides[i].second,
                (i + 1 < limit) ? ", " : "\n");
        }
    }

    // Human-readable inference hints.
    if (reuseRate >= 70.0 && avgReuseDist <= 16.0)
        printf("Inference: strong temporal locality (cache-friendly reuse).\n");
    else if (reuseRate >= 40.0)
        printf("Inference: moderate temporal locality.\n");
    else
        printf("Inference: weak temporal locality (many first touches).\n");

    if (strideSamples > 0)
    {
        double smallStrideRate = 100.0 * (double)(absStrideLE8 + absStride9To64) / (double)strideSamples;
        double boundedCoverage = 100.0 * (double)boundedStrideSamples / (double)strideSamples;

        if (boundedCoverage < 25.0)
            printf("Note: many very large jumps; bounded stride average is more representative.\n");

        if (smallStrideRate >= 70.0)
            printf("Inference: mostly sequential/nearby memory traversal.\n");
        else if ((double)absStrideGT512 * 100.0 / (double)strideSamples >= 40.0)
            printf("Inference: many long jumps/random-like access behavior.\n");
        else
            printf("Inference: mixed spatial locality pattern.\n");
    }
}

int main(int argc, char* argv[])
{
    if (PIN_Init(argc, argv)) return -1;

    trace = fopen(kTracePath, "w");
    if (!trace)
    {
        perror(kTracePath);
        return -1;
    }

    INS_AddInstrumentFunction(Instruction, 0);
    PIN_AddFiniFunction(Fini, 0);

    PIN_StartProgram();
}