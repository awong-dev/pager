/* wf_partial_2in9.h
 *
 * Copied verbatim from docs/reference/wf_partial_2in9.h (same repo) so
 * disp.c can #include it -- firmware/main/CMakeLists.txt's INCLUDE_DIRS is
 * "." only, docs/ is not on the component include path. Do NOT hand-edit
 * this copy; re-copy from docs/reference/wf_partial_2in9.h if that file
 * changes. See that file for the full source citation; short version below.
 *
 * Partial-refresh waveform LUT for a 2.9" 128x296 SSD1680 panel
 * (GoodDisplay GDEY029T94 / GDEM029T94, Waveshare 2.9inch e-Paper Module V2).
 * Bytes [0..152] (153 bytes) go to command 0x32 (Write LUT register) --
 * WF_PARTIAL_2IN9_LUT_LEN below. Bytes [153..158] are separate per-register
 * writes Waveshare's full/fast/gray init paths make but the partial path
 * does not (see the EOPT/VGH/VSH1/VSH2/VSL/VCOM macros below); this driver's
 * `disptest lut 1` partial path only ever sends the first 153.
 *
 * PRIMARY SOURCE (verbatim, order and values preserved):
 *   https://raw.githubusercontent.com/waveshareteam/e-Paper/master/RaspberryPi_JetsonNano/c/lib/e-Paper/EPD_2in9_V2.c
 *   symbol: UBYTE _WF_PARTIAL_2IN9[159]   (file header says "This version: V1.2, Date: 2023-12-21")
 *
 * CORROBORATING SOURCES (byte-for-byte identical to the 159 bytes below):
 *   https://raw.githubusercontent.com/waveshareteam/e-Paper/master/Arduino/epd2in9_V2/epd2in9_V2.cpp
 *   https://raw.githubusercontent.com/waveshareteam/e-Paper/master/STM32/STM32-F103ZET6/User/e-Paper/EPD_2in9_V2.c
 *   GxEPD2 GxEPD2_290_T94_V2::lut_partial[] -- identical to bytes [0..152]:
 *   https://raw.githubusercontent.com/ZinggJM/GxEPD2/master/src/epd/GxEPD2_290_T94_V2.cpp
 *
 * KNOWN DISAGREEMENT (one byte): Waveshare's Python driver has 0x1 where the
 * C drivers have 0x2 at index 66 (Group0 TP/SR/RP row, "RP of Group0" repeat
 * count). The C value (0x2) is used below -- GxEPD2's independently-derived
 * GoodDisplay-based lut_partial also uses 0x2.
 */

#ifndef WF_PARTIAL_2IN9_H
#define WF_PARTIAL_2IN9_H

#include <stdint.h>

/* Full 159-byte array exactly as in Waveshare EPD_2in9_V2.c (_WF_PARTIAL_2IN9[159]).
 * Send only the first 153 bytes to command 0x32. */
static const uint8_t WF_PARTIAL_2IN9[159] =
{
0x0,0x40,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,
0x80,0x80,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,
0x40,0x40,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,
0x0,0x80,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,
0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,
0x0A,0x0,0x0,0x0,0x0,0x0,0x2,
0x1,0x0,0x0,0x0,0x0,0x0,0x0,
0x1,0x0,0x0,0x0,0x0,0x0,0x0,
0x0,0x0,0x0,0x0,0x0,0x0,0x0,
0x0,0x0,0x0,0x0,0x0,0x0,0x0,
0x0,0x0,0x0,0x0,0x0,0x0,0x0,
0x0,0x0,0x0,0x0,0x0,0x0,0x0,
0x0,0x0,0x0,0x0,0x0,0x0,0x0,
0x0,0x0,0x0,0x0,0x0,0x0,0x0,
0x0,0x0,0x0,0x0,0x0,0x0,0x0,
0x0,0x0,0x0,0x0,0x0,0x0,0x0,
0x0,0x0,0x0,0x0,0x0,0x0,0x0,
0x22,0x22,0x22,0x22,0x22,0x22,0x0,0x0,0x0,
0x22,0x17,0x41,0xB0,0x32,0x36,
};

/* Number of bytes to write to command 0x32 (Write LUT register). */
#define WF_PARTIAL_2IN9_LUT_LEN 153

/* Trailing registers derived from the array, written by Waveshare's
 * LUT_by_host() only (full/fast/gray init), NOT by the partial path. Not
 * used by disp.c today (the partial path here only sends the 153-byte LUT),
 * kept for reference/future use exactly as in docs/reference's copy. */
#define WF_PARTIAL_2IN9_EOPT   (WF_PARTIAL_2IN9[153]) /* -> cmd 0x3F        = 0x22 */
#define WF_PARTIAL_2IN9_VGH    (WF_PARTIAL_2IN9[154]) /* -> cmd 0x03        = 0x17 */
#define WF_PARTIAL_2IN9_VSH1   (WF_PARTIAL_2IN9[155]) /* -> cmd 0x04 byte 1 = 0x41 */
#define WF_PARTIAL_2IN9_VSH2   (WF_PARTIAL_2IN9[156]) /* -> cmd 0x04 byte 2 = 0xB0 */
#define WF_PARTIAL_2IN9_VSL    (WF_PARTIAL_2IN9[157]) /* -> cmd 0x04 byte 3 = 0x32 */
#define WF_PARTIAL_2IN9_VCOM   (WF_PARTIAL_2IN9[158]) /* -> cmd 0x2C        = 0x36 */

#endif /* WF_PARTIAL_2IN9_H */
