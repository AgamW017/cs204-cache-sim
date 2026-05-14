#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define N         512
#define HASH_SIZE 4096
#define LIST_LEN  8192

// matrix multiply — strong reuse, LRU and Belady expected to lead.
static float A[N][N], B[N][N], C[N][N];

static void matmul()
{
    float BT[N][N];
    for (int i = 0; i < N; i++)
        for (int j = 0; j < N; j++)
            BT[j][i] = B[i][j];

    for (int i = 0; i < N; i++)
        for (int j = 0; j < N; j++) {
            float s = 0;
            for (int k = 0; k < N; k++)
                s += A[i][k] * BT[j][k];
            C[i][j] = s;
        }
}

// linked list traversal — pointer chasing, all policies expected to struggle equally.
typedef struct Node { int val; struct Node *next; } Node;

static Node pool[LIST_LEN];

static Node *build_list()
{
    int idx[LIST_LEN];
    for (int i = 0; i < LIST_LEN; i++) idx[i] = i;
    for (int i = LIST_LEN - 1; i > 0; i--) {
        int j = rand() % (i + 1);
        int t = idx[i]; idx[i] = idx[j]; idx[j] = t;
    }
    for (int i = 0; i < LIST_LEN - 1; i++) {
        pool[idx[i]].val  = i;
        pool[idx[i]].next = &pool[idx[i + 1]];
    }
    pool[idx[LIST_LEN - 1]].val  = LIST_LEN - 1;
    pool[idx[LIST_LEN - 1]].next = NULL;
    return &pool[idx[0]];
}

static long traverse(Node *head)
{
    long s = 0;
    for (Node *n = head; n; n = n->next) s += n->val;
    return s;
}

// hash table — frequent key reuse, LRU and 2Q expected to lead.
typedef struct Entry { int key, val; struct Entry *next; } Entry;

static Entry  entries[HASH_SIZE * 2];
static Entry *table[HASH_SIZE];
static int    entry_top = 0;

static void ht_insert(int key, int val)
{
    int h      = (unsigned)key % HASH_SIZE;
    Entry *e   = &entries[entry_top++];
    e->key     = key;
    e->val     = val;
    e->next    = table[h];
    table[h]   = e;
}

static int ht_lookup(int key)
{
    int h = (unsigned)key % HASH_SIZE;
    for (Entry *e = table[h]; e; e = e->next)
        if (e->key == key) return e->val;
    return -1;
}

// strided scan — fixed stride pattern, RRIP-SP expected to lead via prefetch.
#define STRIDE_ARR (1 << 19)
static int stride_arr[STRIDE_ARR];

static long strided_sum(int step)
{
    long s = 0;
    for (int i = 0; i < STRIDE_ARR; i += step) s += stride_arr[i];
    return s;
}

// one-shot scan then re-access — single-use polluter, 2Q and DBP expected to lead.
#define SCAN_ARR (1 << 20)
static int scan_arr[SCAN_ARR];

static long one_shot_scan()
{
    long s = 0;
    for (int i = 0; i < SCAN_ARR; i++) s += scan_arr[i];
    return s;
}

int main()
{
    srand(42);

    for (int i = 0; i < N; i++)
        for (int j = 0; j < N; j++) {
            A[i][j] = (float)(i + j + 1);
            B[i][j] = (float)(i - j + 1);
        }

    for (int i = 0; i < STRIDE_ARR; i++) stride_arr[i] = i;
    for (int i = 0; i < SCAN_ARR;   i++) scan_arr[i]   = i;

    long result = 0;

    matmul();
    for (int i = 0; i < N; i++) result += (long)C[i][i];

    Node *head = build_list();
    for (int r = 0; r < 8; r++) result += traverse(head);

    memset(table, 0, sizeof(table));
    for (int i = 0; i < HASH_SIZE * 2; i++) ht_insert(rand() % 100000, i);
    for (int i = 0; i < 50000; i++) result += ht_lookup(rand() % 100000);

    for (int r = 0; r < 4; r++) result += strided_sum(16);

    result += one_shot_scan();
    for (int r = 0; r < 10; r++)
        for (int i = 0; i < N; i++) result += (long)C[i][i];

    printf("result: %ld\n", result);
    return 0;
}