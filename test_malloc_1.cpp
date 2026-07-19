#include <cstddef>
#include <cstdio>
#include <cstring>
#include <unistd.h>

extern void* smalloc(size_t size);

static int g_failures = 0;

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            printf("  FAIL: %s (line %d)\n", #cond, __LINE__);             \
            g_failures++;                                                  \
        }                                                                  \
    } while (0)

static void test_size_zero_returns_null()
{
    printf("test_size_zero_returns_null\n");
    CHECK(smalloc(0) == NULL);
}

static void test_size_too_large_returns_null()
{
    printf("test_size_too_large_returns_null\n");
    CHECK(smalloc(100000001) == NULL);
    CHECK(smalloc((size_t)-1) == NULL);
}

static void test_size_at_max_boundary_succeeds()
{
    printf("test_size_at_max_boundary_succeeds\n");
    void* p = smalloc(100000000);
    CHECK(p != NULL);
}

static void test_happy_path_is_writable()
{
    printf("test_happy_path_is_writable\n");
    const size_t n = 128;
    char* p = (char*)smalloc(n);
    CHECK(p != NULL);
    if (p != NULL) {
        memset(p, 0xAB, n);
        CHECK((unsigned char)p[0] == 0xAB);
        CHECK((unsigned char)p[n - 1] == 0xAB);
    }
}

static void test_consecutive_allocs_are_adjacent()
{
    printf("test_consecutive_allocs_are_adjacent\n");
    const size_t n = 64;
    char* a = (char*)smalloc(n);
    char* b = (char*)smalloc(n);
    CHECK(a != NULL);
    CHECK(b != NULL);
    if (a != NULL && b != NULL) {
        CHECK(b == a + n);
    }
}

static void test_break_does_not_move_on_invalid_request()
{
    printf("test_break_does_not_move_on_invalid_request\n");
    void* before = sbrk(0);
    void* p = smalloc(0);
    void* after = sbrk(0);
    CHECK(p == NULL);
    CHECK(before == after);
}

int main()
{
    test_size_zero_returns_null();
    test_size_too_large_returns_null();
    test_size_at_max_boundary_succeeds();
    test_happy_path_is_writable();
    test_consecutive_allocs_are_adjacent();
    test_break_does_not_move_on_invalid_request();

    if (g_failures == 0) {
        printf("\nALL TESTS PASSED\n");
        return 0;
    }
    printf("\n%d CHECK(S) FAILED\n", g_failures);
    return 1;
}
