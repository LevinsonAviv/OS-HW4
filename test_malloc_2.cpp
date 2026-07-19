#include <cstddef>
#include <cstdio>
#include <cstring>
#include <unistd.h>

extern void *smalloc(size_t size);
extern void *scalloc(size_t num, size_t size);
extern void sfree(void *p);
extern void *srealloc(void *oldp, size_t size);
extern size_t _num_free_blocks();
extern size_t _num_free_bytes();
extern size_t _num_allocated_blocks();
extern size_t _num_allocated_bytes();
extern size_t _num_meta_data_bytes();
extern size_t _size_meta_data();

struct MallocMetadata;
extern struct MallocMetadata *blockListHead;
extern struct MallocMetadata *blockListTail;

static void reset_allocator() {
  blockListHead = NULL;
  blockListTail = NULL;
}

static int g_failures = 0;

#define CHECK(cond)                                                            \
  do {                                                                         \
    if (!(cond)) {                                                             \
      printf("  FAIL: %s (line %d)\n", #cond, __LINE__);                       \
      g_failures++;                                                            \
    }                                                                          \
  } while (0)

static void check_meta_invariant() {
  CHECK(_num_meta_data_bytes() == _num_allocated_blocks() * _size_meta_data());
  CHECK(_num_free_blocks() <= _num_allocated_blocks());
  CHECK(_num_free_bytes() <= _num_allocated_bytes());
}

// ---------- edge cases ----------

static void test_smalloc_edges() {
  printf("test_smalloc_edges\n");
  reset_allocator();
  CHECK(smalloc(0) == NULL);
  CHECK(smalloc(100000001) == NULL);
  CHECK(smalloc((size_t)-1) == NULL);
  CHECK(_num_allocated_blocks() == 0);
  CHECK(_num_allocated_bytes() == 0);
}

static void test_smalloc_max_boundary() {
  printf("test_smalloc_max_boundary\n");
  reset_allocator();
  void *p = smalloc(100000000);
  CHECK(p != NULL);
  CHECK(_num_allocated_bytes() == 100000000);
  if (p != NULL) {
    sfree(p);
  }
}

static void test_sfree_edges() {
  printf("test_sfree_edges\n");
  reset_allocator();
  sfree(NULL);
  CHECK(_num_free_blocks() == 0);

  void *p = smalloc(40);
  CHECK(p != NULL);
  sfree(p);
  CHECK(_num_free_blocks() == 1);
  CHECK(_num_free_bytes() == 40);

  sfree(p); // double free must be a no-op
  CHECK(_num_free_blocks() == 1);
  CHECK(_num_free_bytes() == 40);
}

static void test_srealloc_edges() {
  printf("test_srealloc_edges\n");
  reset_allocator();
  void *p = smalloc(50);
  CHECK(p != NULL);
  CHECK(srealloc(p, 0) == NULL);
  CHECK(srealloc(p, 100000001) == NULL);
  memset(p, 7, 50);
  CHECK(((unsigned char *)p)[49] == 7); // p still valid after failed realloc
  sfree(p);

  void *q = srealloc(NULL, 30); // oldp == NULL behaves like smalloc
  CHECK(q != NULL);
  sfree(q);
}

static void test_scalloc_edges() {
  printf("test_scalloc_edges\n");
  reset_allocator();
  CHECK(scalloc(0, 10) == NULL);
  CHECK(scalloc(10, 0) == NULL);
  CHECK(scalloc(100000001, 1) == NULL);
  CHECK(scalloc(2, 100000000) == NULL); // 2 * 10^8 overflows the limit
  CHECK(_num_allocated_blocks() == 0);
}

// ---------- regular cases ----------

static void test_smalloc_basic_stats() {
  printf("test_smalloc_basic_stats\n");
  reset_allocator();
  void *p = smalloc(100);
  CHECK(p != NULL);
  CHECK(_num_allocated_blocks() == 1);
  CHECK(_num_allocated_bytes() == 100);
  CHECK(_num_free_blocks() == 0);
  CHECK(_num_free_bytes() == 0);
  check_meta_invariant();
  sfree(p);
}

static void test_pointer_layout() {
  printf("test_pointer_layout\n");
  reset_allocator();
  size_t meta = _size_meta_data();
  char *a = (char *)smalloc(64);
  char *b = (char *)smalloc(64);
  CHECK(a != NULL && b != NULL);
  CHECK(_num_allocated_blocks() == 2);
  CHECK(b ==
        a + 64 + (long)meta); // payload starts one metadata past the header
  sfree(a);
  sfree(b);
}

static void test_reuse_no_heap_growth() {
  printf("test_reuse_no_heap_growth\n");
  reset_allocator();
  void *a = smalloc(120);
  CHECK(a != NULL);
  sfree(a);

  void *brk_before = sbrk(0);
  void *b = smalloc(120);
  void *brk_after = sbrk(0);

  CHECK(b == a);                       // reused same block
  CHECK(brk_before == brk_after);      // heap did not grow
  CHECK(_num_allocated_blocks() == 1); // no new block
  sfree(b);
}

static void test_reuse_large_block_for_small() {
  printf("test_reuse_large_block_for_small\n");
  reset_allocator();
  void *big = smalloc(1000);
  CHECK(big != NULL);
  sfree(big);

  void *small = smalloc(10);
  CHECK(small == big);                   // reused the 1000-byte block
  CHECK(_num_free_bytes() == 0);         // whole block marked used, not 10
  CHECK(_num_allocated_bytes() == 1000); // size never changes
  sfree(small);
}

static void test_first_fit_ascending() {
  printf("test_first_fit_ascending\n");
  reset_allocator();
  char *a = (char *)smalloc(80);
  char *b = (char *)smalloc(80);
  char *c = (char *)smalloc(80);
  CHECK(a && b && c);
  CHECK(a < b && b < c);
  sfree(a);
  sfree(c);
  void *r = smalloc(80); // lowest-address free block wins -> a, not c
  CHECK(r == a);
  sfree(r);
  sfree(b);
}

// ---------- complex / integration ----------

static void test_free_middle_of_three() {
  printf("test_free_middle_of_three\n");
  reset_allocator();
  void *b1 = smalloc(200);
  void *b2 = smalloc(200);
  void *b3 = smalloc(200);
  CHECK(b1 && b2 && b3);

  sfree(b2);
  CHECK(_num_free_blocks() == 1);
  CHECK(_num_free_bytes() == 200);
  CHECK(_num_allocated_blocks() == 3); // b1, b3 untouched, block not removed

  void *b2b = smalloc(200);
  CHECK(b2b == b2); // the middle hole is reused
  sfree(b1);
  sfree(b2b);
  sfree(b3);
}

static void test_scalloc_zero_fill() {
  printf("test_scalloc_zero_fill\n");
  reset_allocator();
  size_t n = 256;
  unsigned char *p = (unsigned char *)scalloc(n, 1);
  CHECK(p != NULL);
  if (p != NULL) {
    bool all_zero = true;
    for (size_t i = 0; i < n; i++) {
      if (p[i] != 0) {
        all_zero = false;
        break;
      }
    }
    CHECK(all_zero);
    sfree(p);
  }
}

static void test_scalloc_zeros_reused_dirty_block() {
  printf("test_scalloc_zeros_reused_dirty_block\n");
  reset_allocator();
  unsigned char *p = (unsigned char *)smalloc(128);
  CHECK(p != NULL);
  memset(p, 0xFF, 128);
  sfree(p);
  unsigned char *q = (unsigned char *)scalloc(128, 1);
  CHECK(q == p); // reused the dirty block
  if (q != NULL) {
    bool all_zero = true;
    for (size_t i = 0; i < 128; i++) {
      if (q[i] != 0) {
        all_zero = false;
        break;
      }
    }
    CHECK(all_zero); // scalloc must have re-zeroed it
    sfree(q);
  }
}

static void test_srealloc_shrink_reuses() {
  printf("test_srealloc_shrink_reuses\n");
  reset_allocator();
  char *p = (char *)smalloc(200);
  CHECK(p != NULL);
  memset(p, 0x5A, 200);
  char *q = (char *)srealloc(p, 50);
  CHECK(q == p); // same block reused
  CHECK(_num_allocated_blocks() == 1);
  CHECK((unsigned char)q[0] == 0x5A); // contents intact
  sfree(q);
}

static void test_srealloc_grow_copies_and_frees() {
  printf("test_srealloc_grow_copies_and_frees\n");
  reset_allocator();
  size_t old_n = 64;
  char *p = (char *)smalloc(old_n);
  CHECK(p != NULL);
  for (size_t i = 0; i < old_n; i++) {
    p[i] = (char)(i & 0xFF);
  }

  char *q = (char *)srealloc(p, 300);
  CHECK(q != NULL);
  CHECK(q != p); // grew -> new block
  bool intact = true;
  for (size_t i = 0; i < old_n; i++) {
    if (q[i] != (char)(i & 0xFF)) {
      intact = false;
      break;
    }
  }
  CHECK(intact); // contents copied

  void *r = smalloc(old_n); // old block freed and reusable
  CHECK(r == p);
  sfree(r);
  sfree(q);
}

// ---------- end to end ----------

static void test_e2e_dynamic_array() {
  printf("test_e2e_dynamic_array\n");
  reset_allocator();
  size_t cap = 4;
  size_t len = 0;
  int *arr = (int *)smalloc(cap * sizeof(int));
  CHECK(arr != NULL);

  const size_t N = 1000;
  for (size_t i = 0; i < N; i++) {
    if (len == cap) {
      size_t newcap = cap * 2;
      int *bigger = (int *)srealloc(arr, newcap * sizeof(int));
      CHECK(bigger != NULL);
      if (bigger == NULL)
        return;
      arr = bigger;
      cap = newcap;
    }
    arr[len++] = (int)(i * 3 + 1);
  }

  bool ok = true;
  for (size_t i = 0; i < N; i++) {
    if (arr[i] != (int)(i * 3 + 1)) {
      ok = false;
      break;
    }
  }
  CHECK(ok);
  sfree(arr);
}

// ---------- stress ----------

static unsigned lcg(unsigned &state) {
  state = state * 1103515245u + 12345u;
  return (state >> 16) & 0x7FFF;
}

static void test_stress() {
  printf("test_stress\n");
  reset_allocator();
  const int SLOTS = 128;
  void *ptrs[SLOTS];
  size_t sizes[SLOTS];
  unsigned char tags[SLOTS];
  for (int i = 0; i < SLOTS; i++) {
    ptrs[i] = NULL;
  }

  unsigned rng = 987654321u;
  int live = 0;
  void *brk_prev = sbrk(0);

  const int OPS = 20000;
  for (int op = 0; op < OPS; op++) {
    int idx = lcg(rng) % SLOTS;

    if (ptrs[idx] == NULL) {
      size_t sz = 1 + (lcg(rng) % 512);
      unsigned char tag = (unsigned char)(lcg(rng) & 0xFF);
      void *p = smalloc(sz);
      CHECK(p != NULL);
      if (p != NULL) {
        memset(p, tag, sz);
        ptrs[idx] = p;
        sizes[idx] = sz;
        tags[idx] = tag;
        live++;
      }
    } else {
      unsigned char *p = (unsigned char *)ptrs[idx];
      bool intact = (p[0] == tags[idx] && p[sizes[idx] - 1] == tags[idx]);
      CHECK(intact); // data survived intervening operations

      if (lcg(rng) % 2 == 0) {
        sfree(ptrs[idx]);
        ptrs[idx] = NULL;
        live--;
      } else {
        size_t newsz = 1 + (lcg(rng) % 512);
        unsigned char tag = (unsigned char)(lcg(rng) & 0xFF);
        void *np = srealloc(ptrs[idx], newsz);
        CHECK(np != NULL);
        if (np != NULL) {
          memset(np, tag, newsz);
          ptrs[idx] = np;
          sizes[idx] = newsz;
          tags[idx] = tag;
        }
      }
    }

    CHECK(_num_allocated_blocks() == _num_free_blocks() + (size_t)live);
    void *brk_now = sbrk(0);
    CHECK(brk_now >= brk_prev); // heap never shrinks
    brk_prev = brk_now;
    if (op % 2000 == 0) {
      check_meta_invariant();
    }
  }

  for (int i = 0; i < SLOTS; i++) {
    if (ptrs[i] != NULL) {
      sfree(ptrs[i]);
    }
  }
  CHECK(_num_free_blocks() == _num_allocated_blocks()); // everything freed
  check_meta_invariant();
}

int main() {
  test_smalloc_edges();
  test_sfree_edges();
  test_srealloc_edges();
  test_scalloc_edges();

  test_smalloc_basic_stats();
  test_pointer_layout();
  test_reuse_no_heap_growth();
  test_reuse_large_block_for_small();
  test_first_fit_ascending();

  test_free_middle_of_three();
  test_scalloc_zero_fill();
  test_scalloc_zeros_reused_dirty_block();
  test_srealloc_shrink_reuses();
  test_srealloc_grow_copies_and_frees();

  test_e2e_dynamic_array();
  test_smalloc_max_boundary();
  test_stress();

  if (g_failures == 0) {
    printf("\nALL TESTS PASSED\n");
    return 0;
  }
  printf("\n%d CHECK(S) FAILED\n", g_failures);
  return 1;
}
