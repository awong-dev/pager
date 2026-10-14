/* temp_parse.c -- see temp_parse.h. Pure C, no ESP-IDF. */
#include "temp_parse.h"

#include <string.h>

bool temp_parse_dc(const char *line, int *temp_dc)
{
    if (line == NULL) {
        return false;
    }
    const char *p = strrchr(line, ',');
    if (p == NULL) {
        p = strchr(line, ':');
    }
    p = (p != NULL) ? p + 1 : line;
    while (*p == ' ' || *p == '"') {
        p++;
    }
    bool neg = false;
    if (*p == '-' || *p == '+') {
        neg = (*p == '-');
        p++;
    }
    if (*p < '0' || *p > '9') {
        return false;
    }
    int whole = 0;
    while (*p >= '0' && *p <= '9') {
        if (whole < 10000) {
            whole = whole * 10 + (*p - '0');
        }
        p++;
    }
    int tenth = 0;
    if (*p == '.') {
        p++;
        if (*p >= '0' && *p <= '9') {
            tenth = *p - '0';
        }
        while (*p >= '0' && *p <= '9') {
            p++; // extra digits truncated
        }
    }
    while (*p == ' ' || *p == '"' || *p == '\r' || *p == '\n') {
        p++;
    }
    if (*p != '\0') {
        return false;
    }
    int dc = whole * 10 + tenth;
    if (neg) {
        dc = -dc;
    }
    if (dc < PAGER_TEMP_DC_MIN || dc > PAGER_TEMP_DC_MAX) {
        return false;
    }
    if (temp_dc) {
        *temp_dc = dc;
    }
    return true;
}
