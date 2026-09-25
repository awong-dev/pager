// clockfmt.c — see clockfmt.h's own module comment.

#include "clockfmt.h"

#include <stdio.h>
#include <string.h>

void clockfmt_status_text(bool in_use, bool seeded, int hh, int mm, char *out, size_t out_size)
{
    if (!in_use || !seeded) {
        snprintf(out, out_size, "--:--");
        return;
    }
    snprintf(out, out_size, "%02d:%02d", hh, mm);
}

bool clockfmt_due(const char *cur, char *last, size_t last_size)
{
    if (last_size == 0) {
        return false; // no room to record `last` in -- caller bug, but don't fault
    }
    if (strncmp(cur, last, last_size) == 0) {
        return false;
    }
    strncpy(last, cur, last_size - 1);
    last[last_size - 1] = '\0';
    return true;
}
