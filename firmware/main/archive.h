// archive.h — device-local chat archive (owner request, 10 Oct 2026,
// docs/CHAT_UI_DESIGN.md "Archive"). An archived peer is hidden from Home
// until a message newer than the one that was newest at archive time
// arrives (either direction). Messages are never deleted. Persisted in NVS
// namespace "archive" (one blob, written only on change). UI task only; no
// locking.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "msg.h" // MSG_FROM_MAX only (struct/consts, no ESP-IDF dep)

#define ARCHIVE_MAX 32
#define ARCHIVE_ALIAS_MAX MSG_FROM_MAX

typedef struct {
    char alias[ARCHIVE_ALIAS_MAX];
    int64_t archived_ts; /* the thread's newest message ts at archive time */
} archive_entry_t;

typedef struct {
    int n;
    archive_entry_t e[ARCHIVE_MAX];
} archive_tbl_t;

/* Pure core (host-tested). Mutators return true iff the table changed. */
bool archive_tbl_set(archive_tbl_t *t, const char *alias, int64_t newest_ts);
bool archive_tbl_is_hidden(const archive_tbl_t *t, const char *alias, int64_t newest_ts);
bool archive_tbl_prune(archive_tbl_t *t, const char *alias);

#ifdef ESP_PLATFORM
/* Loads the table from NVS (call once after nvs_flash_init). Power: one NVS read. */
void archive_init(void);
/* Hides `alias` until a message newer than newest_ts. Evicts the oldest
 * archived_ts when full. Power: one NVS blob write if the table changed. */
void archive_set(const char *alias, int64_t newest_ts);
/* True iff an entry exists and newest_ts <= its archived_ts. No I/O. */
bool archive_is_hidden(const char *alias, int64_t newest_ts);
/* Removes the entry (no-op, no write, if absent). Power: one NVS write if removed. */
void archive_prune(const char *alias);
int archive_count(void);
#endif
