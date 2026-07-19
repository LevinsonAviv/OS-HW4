// Rigorous test suite for malloc_3.cpp (Part 3 - buddy allocator + mmap).
//
// Every case is checked with EXACT stat values. The suite leans on a key
// property of a correct buddy allocator: if a test frees everything it
// allocated, coalescing must return the heap to its pristine state
// (32 free blocks of order MAX_ORDER). So each test asserts the pristine
// invariant on entry and exit, plus the exact intermediate stats.
//
// Expected numbers were derived from a reference model and depend only on
// _size_meta_data(); they are recomputed at runtime, not hard-coded, so they
// stay correct if the metadata struct size ever changes.
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <unistd.h>

extern void* smalloc(size_t size);
extern void* scalloc(size_t num, size_t size);
extern void sfree(void* p);
extern void* srealloc(void* oldp, size_t size);
extern size_t _num_free_blocks();
extern size_t _num_free_bytes();
extern size_t _num_allocated_blocks();
extern size_t _num_allocated_bytes();
extern size_t _num_meta_data_bytes();
extern size_t _size_meta_data();

// ---------------------------------------------------------------------------
// harness
// ---------------------------------------------------------------------------
static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond)                                                    \
    do {                                                               \
        ++g_checks;                                                    \
        if (!(cond)) {                                                 \
            printf("  FAIL: %s (line %d)\n", #cond, __LINE__);         \
            ++g_failures;                                              \
        }                                                              \
    } while (0)

// ---------------------------------------------------------------------------
// model helpers (buddy geometry)
// ---------------------------------------------------------------------------
static const size_t MAX_ORDER = 10;
static const size_t INIT_BLOCKS = 32;
static size_t META = 0;   // set from _size_meta_data() in main
static size_t P10 = 0;    // payload of an order-MAX_ORDER block

static size_t btot(size_t order) { return (size_t)128 << order; }        // total block size
static size_t pay(size_t order)  { return btot(order) - META; }          // usable payload

static size_t order_for(size_t req)
{
    size_t need = req + META, o = 0;
    while (btot(o) < need) ++o;
    return o;
}

static size_t sum_pay(size_t lo, size_t hi)   // inclusive; empty if lo>hi
{
    size_t s = 0;
    for (size_t k = lo; k <= hi && lo <= hi; ++k) s += pay(k);
    return s;
}

// Expected stats after a SINGLE allocation of order o out of a pristine heap.
// Splitting one order-MAX_ORDER block down to order o leaves one free buddy at
// each order o..MAX_ORDER-1, plus the one used block of order o.
static size_t exp_ab(size_t o)  { return (INIT_BLOCKS - 1) + 1 + (MAX_ORDER - o); }
static size_t exp_fb(size_t o)  { return (INIT_BLOCKS - 1) + (MAX_ORDER - o); }
static size_t exp_fby(size_t o) { return (INIT_BLOCKS - 1) * P10 + sum_pay(o, MAX_ORDER - 1); }
static size_t exp_aby(size_t o) { return exp_fby(o) + pay(o); }

static void check_stats(size_t ab, size_t fb, size_t aby, size_t fby, const char* where)
{
    if (_num_allocated_blocks() != ab)
        printf("  [%s] alloc_blocks: got %zu want %zu\n", where, _num_allocated_blocks(), ab);
    if (_num_free_blocks() != fb)
        printf("  [%s] free_blocks: got %zu want %zu\n", where, _num_free_blocks(), fb);
    if (_num_allocated_bytes() != aby)
        printf("  [%s] alloc_bytes: got %zu want %zu\n", where, _num_allocated_bytes(), aby);
    if (_num_free_bytes() != fby)
        printf("  [%s] free_bytes: got %zu want %zu\n", where, _num_free_bytes(), fby);
    CHECK(_num_allocated_blocks() == ab);
    CHECK(_num_free_blocks() == fb);
    CHECK(_num_allocated_bytes() == aby);
    CHECK(_num_free_bytes() == fby);
    CHECK(_num_meta_data_bytes() == ab * META);
}

static void check_pristine(const char* where)
{
    check_stats(INIT_BLOCKS, INIT_BLOCKS, INIT_BLOCKS * P10, INIT_BLOCKS * P10, where);
}

// ---------------------------------------------------------------------------
// tests
// ---------------------------------------------------------------------------

static void test_pristine_baseline()
{
    printf("test_pristine_baseline\n");
    check_pristine("baseline");
    CHECK(_size_meta_data() == META);
    CHECK(_size_meta_data() <= 64);          // spec: metadata must be <= 64 bytes
}

static void test_invalid_requests_return_null_and_dont_touch_heap()
{
    printf("test_invalid_requests_return_null_and_dont_touch_heap\n");
    check_pristine("enter");
    CHECK(smalloc(0) == NULL);
    CHECK(smalloc(100000001) == NULL);
    CHECK(smalloc((size_t)-1) == NULL);
    CHECK(scalloc(0, 10) == NULL);
    CHECK(scalloc(10, 0) == NULL);
    CHECK(scalloc(100000001, 1) == NULL);
    CHECK(scalloc(2, 100000000) == NULL);    // product > 10^8
    CHECK(srealloc(NULL, 0) == NULL);
    CHECK(srealloc(NULL, 100000001) == NULL);
    check_pristine("exit");                  // nothing allocated
}

static void test_single_alloc_split_is_exact()
{
    printf("test_single_alloc_split_is_exact\n");
    const size_t reqs[] = {1, 64, 100, 200, 5000, 10000, 30000, 60000};
    for (size_t i = 0; i < sizeof(reqs) / sizeof(reqs[0]); ++i) {
        check_pristine("enter");
        size_t req = reqs[i];
        size_t o = order_for(req);
        void* p = smalloc(req);
        CHECK(p != NULL);
        check_stats(exp_ab(o), exp_fb(o), exp_aby(o), exp_fby(o), "after single alloc");
        // payload is writable across the whole requested range
        memset(p, 0xC3, req);
        CHECK(((unsigned char*)p)[0] == 0xC3);
        CHECK(((unsigned char*)p)[req - 1] == 0xC3);
        sfree(p);
        check_pristine("after free");        // full coalesce back to pristine
    }
}

static void test_tightest_fit_lowest_address()
{
    printf("test_tightest_fit_lowest_address\n");
    check_pristine("enter");
    // Two allocations that need order 0. First splits a block down; the second
    // must take the freed order-0 buddy (tightest fit), NOT split a new block.
    void* a = smalloc(1);
    void* b = smalloc(1);
    CHECK(a != NULL && b != NULL);
    // a and b are order-0 buddies within the same split region.
    size_t diff = (a < b) ? (size_t)((char*)b - (char*)a) : (size_t)((char*)a - (char*)b);
    CHECK(diff == btot(0));                  // exactly one order-0 block apart
    sfree(a);
    sfree(b);
    check_pristine("exit");
}

static void test_reuse_same_block_after_free()
{
    printf("test_reuse_same_block_after_free\n");
    check_pristine("enter");
    void* a = smalloc(128);
    CHECK(a != NULL);
    sfree(a);
    void* b = smalloc(128);
    CHECK(b == a);                           // same address reused, no heap growth
    sfree(b);
    check_pristine("exit");
}

static void test_coalesce_on_free_merges_buddies()
{
    printf("test_coalesce_on_free_merges_buddies\n");
    check_pristine("enter");
    // Allocate two order-8 blocks that become buddies.
    void* a = smalloc(30000);                // order 8
    void* b = smalloc(30000);                // order 8, buddy of a
    CHECK(a != NULL && b != NULL);
    // After two order-8 allocs from pristine:
    //   31 order-10 free, order-9 free @high, a & b used order-8.
    check_stats(34, 32, (INIT_BLOCKS - 1) * P10 + pay(9) + 2 * pay(8),
                (INIT_BLOCKS - 1) * P10 + pay(9), "two order8 used");
    // Free one buddy: must NOT merge (its buddy is still used).
    sfree(a);
    CHECK(_num_free_blocks() == 33);         // a joins the free list, no merge
    // Free the second buddy: now they coalesce all the way back to pristine.
    sfree(b);
    check_pristine("exit");
}

static void test_srealloc_shrink_reuses_same_block()
{
    printf("test_srealloc_shrink_reuses_same_block\n");
    check_pristine("enter");
    char* p = (char*)smalloc(200);           // order 1 (payload 200 fits)
    CHECK(p != NULL);
    memset(p, 0x5A, 200);
    size_t o = order_for(200);
    char* q = (char*)srealloc(p, 50);        // fits current block -> reuse
    CHECK(q == p);
    CHECK(q[0] == (char)0x5A && q[49] == (char)0x5A);
    check_stats(exp_ab(o), exp_fb(o), exp_aby(o), exp_fby(o), "after shrink");
    sfree(q);
    check_pristine("exit");
}

static void test_srealloc_same_size_is_noop()
{
    printf("test_srealloc_same_size_is_noop\n");
    check_pristine("enter");
    void* p = smalloc(64);
    CHECK(p != NULL);
    void* q = srealloc(p, 64);
    CHECK(q == p);
    sfree(q);
    check_pristine("exit");
}

static void test_srealloc_null_behaves_like_malloc()
{
    printf("test_srealloc_null_behaves_like_malloc\n");
    check_pristine("enter");
    size_t o = order_for(40);
    void* p = srealloc(NULL, 40);
    CHECK(p != NULL);
    check_stats(exp_ab(o), exp_fb(o), exp_aby(o), exp_fby(o), "srealloc(NULL)");
    sfree(p);
    check_pristine("exit");
}

static void test_srealloc_zero_and_large_fail_keep_oldp()
{
    printf("test_srealloc_zero_and_large_fail_keep_oldp\n");
    check_pristine("enter");
    char* p = (char*)smalloc(100);
    CHECK(p != NULL);
    memset(p, 0x11, 100);
    CHECK(srealloc(p, 0) == NULL);
    CHECK(srealloc(p, 100000001) == NULL);
    // On failure oldp must be untouched (not freed, data intact).
    CHECK(p[0] == 0x11 && p[99] == 0x11);
    size_t o = order_for(100);
    check_stats(exp_ab(o), exp_fb(o), exp_aby(o), exp_fby(o), "after failed srealloc");
    sfree(p);
    check_pristine("exit");
}

static void test_srealloc_grow_in_place_buddy_merge()
{
    printf("test_srealloc_grow_in_place_buddy_merge\n");
    check_pristine("enter");
    // Allocate one order-8 block. Its order-8 buddy is free. Growing into an
    // order-9 request must merge in place (Challenge 1 / srealloc note 1b),
    // reusing the SAME address rather than allocating a new block.
    char* p = (char*)smalloc(30000);         // order 8
    CHECK(p != NULL);
    for (int i = 0; i < 30000; ++i) p[i] = (char)(i & 0xFF);
    char* q = (char*)srealloc(p, 40000);     // needs order 9
    CHECK(q == p);                           // in-place merge => same pointer
    // data preserved through the merge
    CHECK(q[0] == 0 && q[29999] == (char)(29999 & 0xFF));
    // merged: block@base order9 used, one order9 free, 31 order10 free.
    check_stats(33, 32, (INIT_BLOCKS - 1) * P10 + 2 * pay(9),
                (INIT_BLOCKS - 1) * P10 + pay(9), "after in-place merge");
    sfree(q);
    check_pristine("exit");
}

static void test_srealloc_grow_iterative_buddy_merge()
{
    printf("test_srealloc_grow_iterative_buddy_merge\n");
    check_pristine("enter");
    // order-7 block whose order-7 and order-8 buddies are both free: growing to
    // an order-9 request must merge twice, in place.
    char* p = (char*)smalloc(10000);         // order 7
    CHECK(p != NULL);
    for (int i = 0; i < 10000; ++i) p[i] = (char)((i * 7) & 0xFF);
    char* q = (char*)srealloc(p, 40000);     // needs order 9
    CHECK(q == p);
    CHECK(q[0] == 0 && q[9999] == (char)((9999 * 7) & 0xFF));
    check_stats(33, 32, (INIT_BLOCKS - 1) * P10 + 2 * pay(9),
                (INIT_BLOCKS - 1) * P10 + pay(9), "after iterative merge");
    sfree(q);
    check_pristine("exit");
}

static void test_srealloc_grow_allocates_new_when_buddy_busy()
{
    printf("test_srealloc_grow_allocates_new_when_buddy_busy\n");
    check_pristine("enter");
    char* a = (char*)smalloc(30000);         // order 8 @ base
    char* b = (char*)smalloc(30000);         // order 8, a's buddy (busy)
    CHECK(a != NULL && b != NULL);
    for (int i = 0; i < 30000; ++i) a[i] = (char)((i * 3) & 0xFF);
    // a can't merge (buddy busy) -> allocate a fresh order-9 block, copy, free a.
    char* q = (char*)srealloc(a, 40000);
    CHECK(q != NULL);
    CHECK(q != a && q != b);
    CHECK(q[0] == 0 && q[29999] == (char)((29999 * 3) & 0xFF));  // data copied
    // b used order8; q used order9; a now free order8; 31 order10 free.
    check_stats(34, 32, (INIT_BLOCKS - 1) * P10 + 2 * pay(8) + pay(9),
                (INIT_BLOCKS - 1) * P10 + pay(8), "after new-block realloc");
    sfree(b);
    sfree(q);
    check_pristine("exit");
}

static void test_scalloc_zero_fills_and_stats_exact()
{
    printf("test_scalloc_zero_fills_and_stats_exact\n");
    check_pristine("enter");
    size_t o = order_for(64);
    unsigned char* p = (unsigned char*)scalloc(64, 1);
    CHECK(p != NULL);
    for (int i = 0; i < 64; ++i) CHECK(p[i] == 0);
    check_stats(exp_ab(o), exp_fb(o), exp_aby(o), exp_fby(o), "after scalloc");
    sfree(p);
    check_pristine("exit");
}

static void test_mmap_large_alloc_exact()
{
    printf("test_mmap_large_alloc_exact\n");
    check_pristine("enter");
    const size_t sz = 150 * 1024;            // + meta > 128KB -> mmap
    unsigned char* p = (unsigned char*)smalloc(sz);
    CHECK(p != NULL);
    memset(p, 0xA5, sz);
    CHECK(p[0] == 0xA5 && p[sz - 1] == 0xA5);
    // mmap block counts as allocated, never as free.
    check_stats(INIT_BLOCKS + 1, INIT_BLOCKS,
                INIT_BLOCKS * P10 + sz, INIT_BLOCKS * P10, "one mmap live");
    sfree(p);                                // munmap -> back to pristine
    check_pristine("exit");
}

static void test_two_mmap_blocks_exact()
{
    printf("test_two_mmap_blocks_exact\n");
    check_pristine("enter");
    const size_t sz = 160 * 1024;
    void* a = smalloc(sz);
    void* b = smalloc(sz);
    CHECK(a != NULL && b != NULL);
    check_stats(INIT_BLOCKS + 2, INIT_BLOCKS,
                INIT_BLOCKS * P10 + 2 * sz, INIT_BLOCKS * P10, "two mmap live");
    sfree(a);
    sfree(b);
    check_pristine("exit");
}

static void test_mmap_realloc_allocates_new_block()
{
    printf("test_mmap_realloc_allocates_new_block\n");
    check_pristine("enter");
    const size_t sz = 150 * 1024;
    char* p = (char*)smalloc(sz);
    CHECK(p != NULL);
    for (size_t i = 0; i < sz; ++i) p[i] = (char)(i & 0xFF);
    // Same size -> may reuse.
    CHECK(srealloc(p, sz) == p);
    // Grow -> must be a brand new mmap block (note 4), old data copied over.
    const size_t big = 200 * 1024;
    char* q = (char*)srealloc(p, big);
    CHECK(q != NULL);
    CHECK(q != p);
    CHECK(q[0] == 0 && q[sz - 1] == (char)((sz - 1) & 0xFF));
    check_stats(INIT_BLOCKS + 1, INIT_BLOCKS,
                INIT_BLOCKS * P10 + big, INIT_BLOCKS * P10, "grown mmap block");
    sfree(q);
    check_pristine("exit");
}

static void test_meta_data_invariant()
{
    printf("test_meta_data_invariant\n");
    check_pristine("enter");
    void* a = smalloc(96);
    void* b = smalloc(150 * 1024);           // mmap
    CHECK(a != NULL && b != NULL);
    CHECK(_num_meta_data_bytes() == _num_allocated_blocks() * _size_meta_data());
    sfree(a);
    sfree(b);
    CHECK(_num_meta_data_bytes() == _num_allocated_blocks() * _size_meta_data());
    check_pristine("exit");
}

static void test_max_size_smalloc_boundary()
{
    printf("test_max_size_smalloc_boundary\n");
    check_pristine("enter");
    CHECK(smalloc(100000001) == NULL);       // above 10^8 -> NULL, exactly
    void* p = smalloc(100000000);            // 10^8 -> mmap (best effort)
    CHECK(p != NULL);
    if (p != NULL) {
        ((unsigned char*)p)[0] = 0x7F;
        ((unsigned char*)p)[100000000 - 1] = 0x7F;
        CHECK(((unsigned char*)p)[0] == 0x7F);
        CHECK(_num_allocated_blocks() == INIT_BLOCKS + 1);
        sfree(p);
    }
    check_pristine("exit");
}

static void test_mixed_sequence_returns_to_pristine()
{
    printf("test_mixed_sequence_returns_to_pristine\n");
    check_pristine("enter");
    void* ptrs[16];
    for (int i = 0; i < 16; ++i) {
        ptrs[i] = smalloc((size_t)(i + 1) * 37);
        CHECK(ptrs[i] != NULL);
    }
    // free in a scrambled order to exercise coalescing paths
    int order[16] = {3, 7, 1, 15, 0, 9, 4, 12, 2, 8, 5, 11, 6, 14, 10, 13};
    for (int i = 0; i < 16; ++i) sfree(ptrs[order[i]]);
    check_pristine("exit");                  // everything coalesced back
}

int main()
{
    META = _size_meta_data();
    P10 = btot(MAX_ORDER) - META;

    // Force the one-time initialization (32 free blocks) before asserting the
    // pristine baseline.
    void* warm = smalloc(1);
    if (warm != NULL) sfree(warm);

    test_pristine_baseline();
    test_invalid_requests_return_null_and_dont_touch_heap();
    test_single_alloc_split_is_exact();
    test_tightest_fit_lowest_address();
    test_reuse_same_block_after_free();
    test_coalesce_on_free_merges_buddies();
    test_srealloc_shrink_reuses_same_block();
    test_srealloc_same_size_is_noop();
    test_srealloc_null_behaves_like_malloc();
    test_srealloc_zero_and_large_fail_keep_oldp();
    test_srealloc_grow_in_place_buddy_merge();
    test_srealloc_grow_iterative_buddy_merge();
    test_srealloc_grow_allocates_new_when_buddy_busy();
    test_scalloc_zero_fills_and_stats_exact();
    test_mmap_large_alloc_exact();
    test_two_mmap_blocks_exact();
    test_mmap_realloc_allocates_new_block();
    test_meta_data_invariant();
    test_max_size_smalloc_boundary();
    test_mixed_sequence_returns_to_pristine();

    printf("\n%d checks run\n", g_checks);
    if (g_failures == 0) {
        printf("ALL TESTS PASSED\n");
        return 0;
    }
    printf("%d CHECK(S) FAILED\n", g_failures);
    return 1;
}
