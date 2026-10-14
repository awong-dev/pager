/* temp_parse.h -- pure parser for the modem temperature line (no ESP-IDF, host-tested). */
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* AT command that reads the modem temperature. One-line switch (e.g. to "AT+SMDTH" if the bench
 * shows SQNTMON is not the right source); the parser accepts any reply whose LAST comma-separated
 * field is the temperature, as "+SQNTMON: m,xl,wl,wh,xh,<t>" does. */
#define PAGER_TEMP_CMD "AT+SQNTMON?"

/* Plausible modem temperature window in tenths of a degree C; outside it the reading is rejected
 * (e.g. a modem that printed tenths as an integer would show up as >150 C and be dropped). */
#define PAGER_TEMP_DC_MIN (-400)
#define PAGER_TEMP_DC_MAX 1500

/* Parses the temperature out of a raw response line. Takes the text after the last ',' (or after
 * the first ':' if there is no comma), accepts an optionally signed integer or decimal ("23",
 * "-5", "23.5", "-0.4", "+12.25"); the fraction is truncated toward zero to one digit. Quotes and
 * spaces around the number are ignored. Returns false (leaving *temp_dc unchanged) on no digits,
 * trailing junk, or a value outside [PAGER_TEMP_DC_MIN, PAGER_TEMP_DC_MAX]. */
bool temp_parse_dc(const char *line, int *temp_dc);

#ifdef __cplusplus
}
#endif
