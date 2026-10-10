/* test_archive.c — archive.c's pure table core. */
#include <stdio.h>
#include <string.h>

#include "archive.h"

static int g_failures = 0;
#define CHECK(cond, ...)                                  \
    do {                                                  \
        if (!(cond)) {                                    \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);   \
            printf(__VA_ARGS__);                          \
            printf("\n");                                 \
            g_failures++;                                 \
        }                                                 \
    } while (0)

int main(void)
{
    archive_tbl_t t;
    memset(&t, 0, sizeof(t));

    CHECK(!archive_tbl_is_hidden(&t, "dad", 10), "empty table hides nothing");
    CHECK(archive_tbl_set(&t, "dad", 100), "set changes table");
    CHECK(!archive_tbl_set(&t, "dad", 100), "same set is a no-op (no NVS write)");
    CHECK(t.n == 1, "n==1");
    CHECK(archive_tbl_is_hidden(&t, "dad", 100), "newest==archived -> hidden");
    CHECK(archive_tbl_is_hidden(&t, "dad", 50), "older -> hidden");
    CHECK(!archive_tbl_is_hidden(&t, "dad", 101), "newer ts unhides");
    CHECK(!archive_tbl_is_hidden(&t, "mom", 1), "other alias unaffected");
    CHECK(archive_tbl_set(&t, "dad", 200), "re-archive updates ts");
    CHECK(t.n == 1 && archive_tbl_is_hidden(&t, "dad", 150), "updated ts hides");
    CHECK(archive_tbl_prune(&t, "dad"), "prune removes");
    CHECK(!archive_tbl_prune(&t, "dad"), "second prune is a no-op");
    CHECK(t.n == 0 && !archive_tbl_is_hidden(&t, "dad", 1), "gone after prune");

    /* eviction: fill 32 with ts 1000+i, add a 33rd -> oldest (p5, ts 5) goes */
    memset(&t, 0, sizeof(t));
    for (int i = 0; i < ARCHIVE_MAX; i++) {
        char a[8];
        snprintf(a, sizeof(a), "p%d", i);
        archive_tbl_set(&t, a, i == 5 ? 5 : 1000 + i);
    }
    CHECK(t.n == ARCHIVE_MAX, "full at 32");
    CHECK(archive_tbl_set(&t, "new", 2000), "33rd set");
    CHECK(t.n == ARCHIVE_MAX, "still 32");
    CHECK(!archive_tbl_is_hidden(&t, "p5", 1), "oldest evicted");
    CHECK(archive_tbl_is_hidden(&t, "p0", 1000), "others kept");
    CHECK(archive_tbl_is_hidden(&t, "new", 2000), "new present");

    if (g_failures == 0) {
        printf("PASS: all test_archive checks passed\n");
        return 0;
    }
    printf("FAILED: %d check(s) failed\n", g_failures);
    return 1;
}
