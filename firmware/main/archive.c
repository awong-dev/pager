// archive.c — see archive.h. Pure table core on top, NVS-backed singleton in
// the ESP_PLATFORM section (same split as scr_home.c/auth.c).

#include "archive.h"

#include <string.h>

static int find(const archive_tbl_t *t, const char *alias)
{
    for (int i = 0; i < t->n; i++) {
        if (strncmp(t->e[i].alias, alias, ARCHIVE_ALIAS_MAX - 1) == 0) {
            return i;
        }
    }
    return -1;
}

bool archive_tbl_set(archive_tbl_t *t, const char *alias, int64_t newest_ts)
{
    if (!alias || alias[0] == '\0') {
        return false;
    }
    int i = find(t, alias);
    if (i >= 0) {
        if (t->e[i].archived_ts == newest_ts) {
            return false;
        }
        t->e[i].archived_ts = newest_ts;
        return true;
    }
    if (t->n >= ARCHIVE_MAX) {
        int oldest = 0;
        for (int k = 1; k < t->n; k++) {
            if (t->e[k].archived_ts < t->e[oldest].archived_ts) {
                oldest = k;
            }
        }
        t->e[oldest] = t->e[t->n - 1];
        t->n--;
    }
    archive_entry_t *e = &t->e[t->n++];
    memset(e, 0, sizeof(*e));
    strncpy(e->alias, alias, ARCHIVE_ALIAS_MAX - 1);
    e->archived_ts = newest_ts;
    return true;
}

bool archive_tbl_is_hidden(const archive_tbl_t *t, const char *alias, int64_t newest_ts)
{
    int i = alias ? find(t, alias) : -1;
    return i >= 0 && newest_ts <= t->e[i].archived_ts;
}

bool archive_tbl_prune(archive_tbl_t *t, const char *alias)
{
    int i = alias ? find(t, alias) : -1;
    if (i < 0) {
        return false;
    }
    t->e[i] = t->e[t->n - 1];
    t->n--;
    return true;
}

#ifdef ESP_PLATFORM

#include "nvs.h"

#define ARCHIVE_NS "archive"
#define ARCHIVE_BLOB_VERSION 1u

typedef struct {
    uint32_t layout;
    archive_tbl_t tbl;
} archive_blob_t;

static archive_tbl_t s_tbl;

void archive_init(void)
{
    memset(&s_tbl, 0, sizeof(s_tbl));
    nvs_handle_t h;
    if (nvs_open(ARCHIVE_NS, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    archive_blob_t tmp;
    size_t len = sizeof(tmp);
    if (nvs_get_blob(h, "blob", &tmp, &len) == ESP_OK && len == sizeof(tmp) &&
        tmp.layout == ARCHIVE_BLOB_VERSION && tmp.tbl.n >= 0 && tmp.tbl.n <= ARCHIVE_MAX) {
        s_tbl = tmp.tbl;
    }
    nvs_close(h);
}

// Power effect: one NVS blob write (~1 kB), only called on change.
static void persist(void)
{
    archive_blob_t b;
    memset(&b, 0, sizeof(b));
    b.layout = ARCHIVE_BLOB_VERSION;
    b.tbl = s_tbl;
    nvs_handle_t h;
    if (nvs_open(ARCHIVE_NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    if (nvs_set_blob(h, "blob", &b, sizeof(b)) == ESP_OK) {
        nvs_commit(h);
    }
    nvs_close(h);
}

void archive_set(const char *alias, int64_t newest_ts)
{
    if (archive_tbl_set(&s_tbl, alias, newest_ts)) {
        persist();
    }
}

bool archive_is_hidden(const char *alias, int64_t newest_ts)
{
    return archive_tbl_is_hidden(&s_tbl, alias, newest_ts);
}

void archive_prune(const char *alias)
{
    if (archive_tbl_prune(&s_tbl, alias)) {
        persist();
    }
}

int archive_count(void) { return s_tbl.n; }

#endif /* ESP_PLATFORM */
