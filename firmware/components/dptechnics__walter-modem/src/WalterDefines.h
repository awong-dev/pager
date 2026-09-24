/**
 * @file WalterDefines.h
 * @author Daan Pape <daan@dptechnics.com>
 * @author Arnoud Devoogdt <arnoud@dptechnics.com>
 * @date 16 January 2026
 * @version 1.5.0
 * @copyright DPTechnics bv <info@dptechnics.com>
 * @brief Walter Modem library
 *
 * @section LICENSE
 *
 * Copyright (C) 2026, DPTechnics bv
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without modification, are permitted
 * provided that the following conditions are met:
 *
 *   1. Redistributions of source code must retain the above copyright notice, this list of
 *      conditions and the following disclaimer.
 *
 *   2. Redistributions in binary form must reproduce the above copyright notice, this list of
 *      conditions and the following disclaimer in the documentation and/or other materials provided
 *      with the distribution.
 *
 *   3. Neither the name of DPTechnics bv nor the names of its contributors may be used to endorse
 *      or promote products derived from this software without specific prior written permission.
 *
 *   4. This software, with or without modification, must only be used with a Walter board from
 *      DPTechnics bv.
 *
 *   5. Any software provided in binary form under this license must not be reverse engineered,
 *      decompiled, modified and/or disassembled.
 *
 * THIS SOFTWARE IS PROVIDED BY DPTECHNICS BV “AS IS” AND ANY EXPRESS OR IMPLIED WARRANTIES,
 * INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY, NONINFRINGEMENT, AND
 * FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL DPTECHNICS BV OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA,
 * OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
 * THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * @section DESCRIPTION
 *
 * This file contains the headers and common defines of Walter's modem library.
 */

#ifndef WALTER_DEFINES_H
#define WALTER_DEFINES_H
#include <WalterModem.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include <chrono> // PAGER PATCH: 1.10

// PAGER PATCH: 1.10 — strong-overridden in firmware/main/watchdog.c so a
// synchronous command wait that runs long (a slow AT+COPS=0 during a network
// search, holding the queue for up to 3 retries * 30 s
// CONFIG_WALTER_MODEM_CMD_TIMEOUT_MS = 90 s) keeps both watchdogs fed instead
// of blocking silently past the 60 s task watchdog timeout. Weak default is
// defined once in WalterModem.cpp so this component still builds standalone.
extern "C" void walter_modem_block_tick(void);

// PAGER PATCH: (RCA_SLEEP_PUBLISH.md §3 / PATCHES.md 1.12) four free-running,
// never-reset counters for the "orphaned > data prompt" investigation. All
// four are read-only outside this component; firmware/main/net.cpp exposes
// them to modes.c's sleeptest report (the only thing that survives the
// USB-dead light-sleep window) via walter_modem_pager_counters(). No
// behaviour change: increments only, no extra AT traffic, no power effect.
//   datatx_retx    - WalterModem.cpp's _processModemCMD(): incremented each
//                    time a DATA_TX_WAIT command's timeout sends the payload
//                    bytes instead of re-transmitting the AT command line
//                    (patch 1.12).
//   prompt_orphan  - WalterModem.cpp's _parseRxData(): incremented each time
//                    the bare, already-stripped "> " prompt (patch 1.11's
//                    prompt3 case) is recognised -- the discriminator for
//                    whether the orphaned-prompt mechanism (RCA §2) is what
//                    is actually happening on the bench.
//   buf_drop_queue - WalterModem.cpp's _queueRxBuffer(): incremented when the
//                    8-slot _taskQueue is full and a fully-parsed buffer is
//                    dropped instead of queued.
//   buf_drop_pool  - WalterModem.cpp's _getFreeBuffer(): incremented when the
//                    8-buffer pool is exhausted and a buffer allocation fails.
// PAGER PATCH: 1.13 (docs/RCA_SLEEP_URC.md §5 fix 3-4, docs/SLEEP_URC_TASKS.md
// S3). Attribution for the two 30s stalls RCA_SLEEP_URC.md §1 could not tell
// apart: which write path actually moved bytes, whether uart_wait_tx_done()
// ever timed out, and -- for the last command that stalled at least 5s --
// which command it was.
//   prompt_handled         - WalterModem.cpp's "> " prompt handler: the
//                            payload write actually ran (a DATA_TX_WAIT
//                            command with a payload was current).
//   payload_bytes_written  - total bytes actually written by that payload
//                            write (uart_write_bytes()'s own return value,
//                            previously discarded), summed with patch 1.12's
//                            timeout-triggered payload write -- both are
//                            "the payload write" this counter attributes.
//   txdone_timeouts        - WalterModem.cpp's _uartWrite(): count of times
//                            uart_wait_tx_done()'s 10ms wait did NOT return
//                            ESP_OK (previously discarded entirely).
//   stall_cmd/stall_elapsed_ms/stall_cts_level/stall_tx_ring_bytes - a
//                            snapshot of the most recent command whose
//                            current attempt had been outstanding for >= 5s
//                            when _processModemCMD() last re-evaluated it
//                            (see WalterModem::_pagerSnapshotStall()):
//                            stall_cmd is the first 24 characters of its AT
//                            command line (NUL-terminated); stall_cts_level
//                            is gpio_get_level() on the CTS pin at that
//                            moment (-1 if unavailable); stall_tx_ring_bytes
//                            is uart_get_tx_buffer_free_size()'s "free"
//                            count -- this UART is installed with a 0-byte
//                            TX ring (WalterModem.cpp's own
//                            uart_driver_install() call), so this reads 0 on
//                            this target; txdone_timeouts above is the real
//                            discriminator for a wire-level stall, this
//                            field is included only because the task asked
//                            for it. Overwritten on every such observation,
//                            so it reflects the most recently seen slow
//                            command, not necessarily one still stalled.
// PAGER PATCH: 1.15 (docs/SLEEP_URC_DESIGN.md §8.2, docs/SLEEP_URC_TASKS.md
// S10). Counters, not a trace -- light sleep kills the USB CDC
// (firmware/main/modes.c), so there is no in-window AT log to read back; these
// two discriminate the three surviving explanations for the 30 s
// "stalled command:" stalls without capturing a byte.
//   rsp_no_cmd      - WalterModem.cpp's _processModemRSP(): incremented when a
//                      buffer reaches the function's completion test with
//                      cmd == NULL and result == OK, i.e. it falls through
//                      unused (RSP_PROC_FINISH region) instead of completing a
//                      command or being claimed by an earlier
//                      "if (cmd == NULL) return" branch. Hypothesis 1
//                      (response/command desync inside a URC flush burst)
//                      predicts >= 1 of these per stall; hypotheses 2
//                      (_receivingPayload sticks) and 3 (modem genuinely
//                      silent) predict 0. Free-running, never reset.
//   payload_stuck_ms - WalterModem.cpp's _processModemCMD(): how long
//                      _receivingPayload had already been true when a command
//                      timed out (the TX_WAIT/DATA_TX_WAIT and WAIT timeout
//                      sites). Hypothesis 2 predicts > 0; 1 and 3 predict 0.
//                      Overwritten on every such observation, so it reflects
//                      the most recently observed stuck-payload timeout, not
//                      necessarily one still stuck.
// PAGER PATCH: 1.18 (docs/SLEEP_PAGE_LOSS_BRIEF.md, build/bench-logs/
// phaseBB-report.log c01/c08). The stray 0xFF byte the modem UART delivers
// at the start of every observed light-sleep wake, before anything else.
//   glitch_dropped - WalterModem.cpp's _parseRxData(): incremented each time
//                     that leading 0xFF is dropped (non-payload path, empty
//                     parser buffer only -- see the patch comment at the
//                     drop site). Free-running, never reset.
typedef struct {
  uint32_t datatx_retx;
  uint32_t prompt_orphan;
  uint32_t buf_drop_queue;
  uint32_t buf_drop_pool;
  uint32_t prompt_handled;
  uint32_t payload_bytes_written;
  uint32_t txdone_timeouts;
  char stall_cmd[25];
  uint32_t stall_elapsed_ms;
  int32_t stall_cts_level;
  uint32_t stall_tx_ring_bytes;
  uint32_t rsp_no_cmd;
  uint32_t payload_stuck_ms;
  uint32_t glitch_dropped;
} walter_modem_pager_counters_t;

extern "C" walter_modem_pager_counters_t walter_modem_pager_counters(void);

// NOLINT(readability-identifier-naming.PrivateFunctionPrefix)
/**
 * @brief Convert a digit to a string literal.
 *
 * @param val The value to convert.
 * @return The resulting string literal or an empty string when the value is not in [0,9].
 */
const char* _digitStr(int val);

/**
 * @brief Convert a digit at a certain position in a number to a string literal.
 *
 * The position starts at 0 (the leftmost digit).
 *
 * @param num The number.
 * @param pos The digit position (0-based from left).
 * @return The string literal representing the digit, or an empty string if pos is out-of-range.
 */
const char* _intToStrDigit(int num, int pos);

/**
 * @brief Convert a PDP type to a string literal.
 *
 * @param type The PDP type.
 * @return The resulting string literal.
 */
const char* _pdpTypeStr(WalterModemPDPType type);

/**
 * @brief Convert a time string to a Unix timestamp.
 *
 * **Note:** No time zone conversion is performed. Provide a time string in the time zone
 * you expect and handle offsets externally.
 *
 * @param timeStr The time string.
 * @param format The time format to parse. (For example, "%Y-%m-%dT%H:%M:%S")
 * @return The Unix timestamp on success, or -1 on error.
 */
int64_t strTotime(const char* timeStr, const char* format);

/**
 * @brief Convert a unix timestamp to a formatted time string.
 *
 * The time string is in UTC (no time zone offset applied).
 *
 * @param timestamp The unix timestamp to convert.
 * @param buffer The output buffer to hold the formatted string.
 * @param buffer_len The length of the output buffer.
 * @param format The time format to use (default: "%Y-%m-%dT%H:%M:%S").
 *
 * @return true on success, false on error.
 */
bool timeToStr(uint64_t timestamp, char* buffer, size_t buffer_len,
               const char* format = "%Y-%m-%dT%H:%M:%S");

/**
 * @brief Convert a string into an unsigned 32-bit integer.
 *
 * @param str The null-terminated string.
 * @param len The length of the string, or -1 to use strlen.
 * @param result Pointer to store the result.
 * @param radix Conversion radix (e.g., 10).
 * @param max Maximum allowed value (e.g., UINT32_MAX).
 * @return true if conversion was successful; false otherwise.
 */
bool strToUint32(const char* str, int len, uint32_t* result, int radix, uint32_t max);

/**
 * @brief Convert a string into an unsigned 16-bit integer.
 *
 * @param str The null-terminated string.
 * @param len The length of the string, or -1 to use strlen.
 * @param result Pointer to store the result.
 * @param radix Conversion radix (e.g., 10).
 * @return true if conversion was successful; false otherwise.
 */
bool strToUint16(const char* str, int len, uint16_t* result, int radix);

/**
 * @brief Convert a string into an unsigned 8-bit integer.
 *
 * @param str The null-terminated string.
 * @param len The length of the string, or -1 to use strlen.
 * @param result Pointer to store the result.
 * @param radix Conversion radix (e.g., 10).
 * @return true if conversion was successful; false otherwise.
 */
bool strToUint8(const char* str, int len, uint8_t* result, int radix);

/**
 * @brief Convert a string into an IEEE754 float.
 *
 * @param str The null-terminated string.
 * @param len The length of the string, or -1 to use strlen.
 * @param result Pointer to store the float result.
 * @return true if conversion was successful; false otherwise.
 */
bool strToFloat(const char* str, int len, float* result);

#define WALTER_DEFINES_H
/**
 * @brief The length of a string literal at compile time.
 */
#define _strLitLen(str) (sizeof(str) - 1)

/**
 * @brief Check if a WalterModemBuffer starts with a given string literal.
 */
#define _buffStartsWith(buff, str)                                                                 \
  ((buff->size >= _strLitLen(str)) && memcmp(str, buff->data, _strLitLen(str)) == 0)

/**
 * @brief Check if a WalterModemBuffer starts with an ASCII digit [0-9].
 */
#define _buffStartsWithDigit(buff)                                                                 \
  ((buff->size > 0) && (buff->data[0] >= '0' && buff->data[0] <= '9'))

/**
 * @brief 0-terminate a WalterModemBuffer. This macro is meant to be used as an assignment in
 * the form of x = _buffStr(buff);
 */
#define _buffStr(buff)                                                                             \
  (const char*) buff->data;                                                                        \
  buff->data[buff->size] = '\0'

/**
 * Check if the data string in a buffer equals an expected value which comes after a prefix.
 */
#define _dataStrIs(buff, prefix, expected)                                                         \
  ((buff->size - _strLitLen(prefix)) > 0 &&                                                        \
   memcmp(buff->data + _strLitLen(prefix), expected, buff->size - _strLitLen(prefix)) == 0)

/**
 * @brief Convert a string literal into an AT command array. An empty string will result in "","",""
 * and an non-empty string will result in "\"", "<string>", "\"".
 */
#define _atStr(str) strlen(str) > 0 ? "\"" : "", str, strlen(str) > 0 ? "\"" : ""

/**
 * @brief Convert a boolean flag into a string literal.
 */
#define _atBool(bool) bool ? "1" : "0"

/**
 * @brief Convert a number of up to 6 digits in length into an array of digits which can be passed
 * to an AT command array.
 */
#define _atNum(num)                                                                                \
  _intToStrDigit(num, 0), _intToStrDigit(num, 1), _intToStrDigit(num, 2), _intToStrDigit(num, 3),  \
      _intToStrDigit(num, 4), _intToStrDigit(num, 5)

/**
 * @brief Perform a string copy and ensure that the destination is 0-terminated.
 */
#define _strncpy_s(dst, src, max)                                                                  \
  strncpy(dst, src == NULL ? "" : src, max);                                                       \
  dst[max - 1] = '\0';

/**
 * @brief Make an array of a list of arguments.
 */
#define arr(...) { __VA_ARGS__ }

/**
 * @brief Return an error.
 */
#define _returnState(state)                                                                        \
  if(cb == NULL) {                                                                                 \
    if(rsp != NULL) {                                                                              \
      rsp->result = state;                                                                         \
    }                                                                                              \
    return state == WALTER_MODEM_STATE_OK;                                                         \
  } else {                                                                                         \
    WalterModemRsp cbRsp = {};                                                                     \
    cbRsp.result = state;                                                                          \
    cb(&cbRsp, args);                                                                              \
    return true;                                                                                   \
  }

/**
 * @brief Convert little endian to big endian for 16-bit integers.
 */
#define _switchEndian16(x) ((((x) << 8) | ((x) >> 8)) & 0xffff)

/**
 * @brief Convert little endian to big endian
 */
#define _switchEndian32(x)                                                                         \
  ((((x) << 24) | (((x) << 8) & 0x00ff0000) | (((x) >> 8) & 0x0000ff00) | ((x) >> 24)) & 0xffffffff)

/**
 * @brief Add a command to the queue for it to be execute it.
 *
 * This macro will add a command to the command queue for it to be executed. If the command could
 * not be added to the queue the macro will make the surrounding function return false (when
 * blocking API is used) or call the callback with an out-of-memory error.
 *
 * @param atCmd The NULL terminated AT command elements to send to the modem.
 * @param atRsp The expected AT response from the modem.
 * @param rsp Pointer to the response structure to save the response in.
 * @param cb Optional user callback for asynchronous API.
 * @param[in] args Arguments to pass to the callback.
 * @param ... Optional arguments for the _queueModemCMD function.
 */
#define _runCmd(atCmd, atRsp, rsp, cb, args, ...)                                                  \
  const char* _cmdArr[WALTER_MODEM_COMMAND_MAX_ELEMS + 1] = atCmd;                                 \
  WalterModemCmd* cmd = _queueModemCMD(_cmdArr, atRsp, rsp, cb, args, ##__VA_ARGS__);              \
  if(cmd == NULL) {                                                                                \
    _returnState(WALTER_MODEM_STATE_NO_MEMORY);                                                    \
  }                                                                                                \
  std::unique_lock<std::mutex> lock { cmd->cmdLock.mutex };

/**
 * @brief When working asynchronously, it will return true because the response is handled through
 * the user callback. If the blocking API is being used and the expected command state is reached,
 * the mutex is released, the correct result of the command is passed and the function returns with
 * true when the state is WALTER_MODEM_STATE_OK, false otherwise.
 */
#define _returnAfterReply()                                                                        \
  if(cmd->userCb != NULL) {                                                                        \
    lock.unlock();                                                                                 \
    return true;                                                                                   \
  }                                                                                                \
  /* PAGER PATCH: 1.10 */                                                                          \
  while(!cmd->cmdLock.cond.wait_for(lock, std::chrono::milliseconds(1000), [cmd] {                 \
    return cmd->state == WALTER_MODEM_CMD_STATE_SYNC_LOCK_NOTIFIED;                                \
  })) {                                                                                             \
    walter_modem_block_tick();                                                                     \
  }                                                                                                 \
  WalterModemState rspResult = cmd->rsp->result;                                                   \
  cmd->state = WALTER_MODEM_CMD_STATE_COMPLETE;                                                    \
  lock.unlock();                                                                                   \
  return rspResult == WALTER_MODEM_STATE_OK;

/**
 * @brief Transmit all elements of a command.
 */
static bool endOfLine DISABLE_USED_WARNING;
#ifdef ARDUINO
#define _transmitCmd(type, atCmd)                                                                  \
  {                                                                                                \
    ESP_LOGD((endOfLine = false, "WalterModem"),                                                   \
             "TX: %s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s"                \
             "%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s",                     \
             (!endOfLine && atCmd[0]) ? atCmd[0] : (endOfLine = true, ""),                         \
             (!endOfLine && atCmd[1]) ? atCmd[1] : (endOfLine = true, ""),                         \
             (!endOfLine && atCmd[2]) ? atCmd[2] : (endOfLine = true, ""),                         \
             (!endOfLine && atCmd[3]) ? atCmd[3] : (endOfLine = true, ""),                         \
             (!endOfLine && atCmd[4]) ? atCmd[4] : (endOfLine = true, ""),                         \
             (!endOfLine && atCmd[5]) ? atCmd[5] : (endOfLine = true, ""),                         \
             (!endOfLine && atCmd[6]) ? atCmd[6] : (endOfLine = true, ""),                         \
             (!endOfLine && atCmd[7]) ? atCmd[7] : (endOfLine = true, ""),                         \
             (!endOfLine && atCmd[8]) ? atCmd[8] : (endOfLine = true, ""),                         \
             (!endOfLine && atCmd[9]) ? atCmd[9] : (endOfLine = true, ""),                         \
             (!endOfLine && atCmd[10]) ? atCmd[10] : (endOfLine = true, ""),                       \
             (!endOfLine && atCmd[11]) ? atCmd[11] : (endOfLine = true, ""),                       \
             (!endOfLine && atCmd[12]) ? atCmd[12] : (endOfLine = true, ""),                       \
             (!endOfLine && atCmd[13]) ? atCmd[13] : (endOfLine = true, ""),                       \
             (!endOfLine && atCmd[14]) ? atCmd[14] : (endOfLine = true, ""),                       \
             (!endOfLine && atCmd[15]) ? atCmd[15] : (endOfLine = true, ""),                       \
             (!endOfLine && atCmd[16]) ? atCmd[16] : (endOfLine = true, ""),                       \
             (!endOfLine && atCmd[17]) ? atCmd[17] : (endOfLine = true, ""),                       \
             (!endOfLine && atCmd[18]) ? atCmd[18] : (endOfLine = true, ""),                       \
             (!endOfLine && atCmd[19]) ? atCmd[19] : (endOfLine = true, ""),                       \
             (!endOfLine && atCmd[20]) ? atCmd[20] : (endOfLine = true, ""),                       \
             (!endOfLine && atCmd[21]) ? atCmd[21] : (endOfLine = true, ""),                       \
             (!endOfLine && atCmd[22]) ? atCmd[22] : (endOfLine = true, ""),                       \
             (!endOfLine && atCmd[23]) ? atCmd[23] : (endOfLine = true, ""),                       \
             (!endOfLine && atCmd[24]) ? atCmd[24] : (endOfLine = true, ""),                       \
             (!endOfLine && atCmd[25]) ? atCmd[25] : (endOfLine = true, ""),                       \
             (!endOfLine && atCmd[26]) ? atCmd[26] : (endOfLine = true, ""),                       \
             (!endOfLine && atCmd[27]) ? atCmd[27] : (endOfLine = true, ""),                       \
             (!endOfLine && atCmd[28]) ? atCmd[28] : (endOfLine = true, ""),                       \
             (!endOfLine && atCmd[29]) ? atCmd[29] : (endOfLine = true, ""),                       \
             (!endOfLine && atCmd[30]) ? atCmd[30] : (endOfLine = true, ""),                       \
             (!endOfLine && atCmd[31]) ? atCmd[31] : (endOfLine = true, ""),                       \
             (!endOfLine && atCmd[32]) ? atCmd[32] : (endOfLine = true, ""),                       \
             (!endOfLine && atCmd[33]) ? atCmd[33] : (endOfLine = true, ""),                       \
             (!endOfLine && atCmd[34]) ? atCmd[34] : (endOfLine = true, ""),                       \
             (!endOfLine && atCmd[35]) ? atCmd[35] : (endOfLine = true, ""),                       \
             (!endOfLine && atCmd[36]) ? atCmd[36] : (endOfLine = true, ""),                       \
             (!endOfLine && atCmd[37]) ? atCmd[37] : (endOfLine = true, ""),                       \
             (!endOfLine && atCmd[38]) ? atCmd[38] : (endOfLine = true, ""),                       \
             (!endOfLine && atCmd[39]) ? atCmd[39] : (endOfLine = true, ""),                       \
             (!endOfLine && atCmd[40]) ? atCmd[40] : (endOfLine = true, ""),                       \
             (!endOfLine && atCmd[41]) ? atCmd[41] : (endOfLine = true, ""),                       \
             (!endOfLine && atCmd[42]) ? atCmd[42] : (endOfLine = true, ""),                       \
             (!endOfLine && atCmd[43]) ? atCmd[43] : (endOfLine = true, ""),                       \
             (!endOfLine && atCmd[44]) ? atCmd[44] : (endOfLine = true, ""),                       \
             (!endOfLine && atCmd[45]) ? atCmd[45] : (endOfLine = true, ""),                       \
             (!endOfLine && atCmd[46]) ? atCmd[46] : (endOfLine = true, ""),                       \
             (!endOfLine && atCmd[47]) ? atCmd[47] : (endOfLine = true, ""),                       \
             (!endOfLine && atCmd[48]) ? atCmd[48] : (endOfLine = true, ""),                       \
             (!endOfLine && atCmd[49]) ? atCmd[49] : (endOfLine = true, ""),                       \
             (!endOfLine && atCmd[50]) ? atCmd[50] : (endOfLine = true, ""),                       \
             (!endOfLine && atCmd[51]) ? atCmd[51] : (endOfLine = true, ""),                       \
             (!endOfLine && atCmd[52]) ? atCmd[52] : (endOfLine = true, ""),                       \
             (!endOfLine && atCmd[53]) ? atCmd[53] : (endOfLine = true, ""),                       \
             (!endOfLine && atCmd[54]) ? atCmd[54] : (endOfLine = true, ""),                       \
             (!endOfLine && atCmd[55]) ? atCmd[55] : (endOfLine = true, ""),                       \
             (!endOfLine && atCmd[56]) ? atCmd[56] : (endOfLine = true, ""),                       \
             (!endOfLine && atCmd[57]) ? atCmd[57] : (endOfLine = true, ""),                       \
             (!endOfLine && atCmd[58]) ? atCmd[58] : (endOfLine = true, ""),                       \
             (!endOfLine && atCmd[59]) ? atCmd[59] : (endOfLine = true, ""),                       \
             (!endOfLine && atCmd[60]) ? atCmd[60] : (endOfLine = true, ""),                       \
             (!endOfLine && atCmd[61]) ? atCmd[61] : (endOfLine = true, ""),                       \
             (!endOfLine && atCmd[62]) ? atCmd[62] : (endOfLine = true, ""));                      \
    for(int i = 0; i < WALTER_MODEM_COMMAND_MAX_ELEMS; ++i) {                                      \
      if(atCmd[i] == NULL) {                                                                       \
        break;                                                                                     \
      }                                                                                            \
      _uart->write(atCmd[i]);                                                                      \
    }                                                                                              \
    _uart->write(type == WALTER_MODEM_CMD_TYPE_DATA_TX_WAIT ? "\n" : "\r\n");                      \
  }
#else
#define _transmitCmd(type, atCmd)                                                                  \
  {                                                                                                \
    ESP_LOGD((endOfLine = false, "WalterModem"),                                                   \
             "TX: %s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s"                \
             "%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s",                     \
             (!endOfLine && atCmd[0]) ? atCmd[0] : (endOfLine = true, ""),                         \
             (!endOfLine && atCmd[1]) ? atCmd[1] : (endOfLine = true, ""),                         \
             (!endOfLine && atCmd[2]) ? atCmd[2] : (endOfLine = true, ""),                         \
             (!endOfLine && atCmd[3]) ? atCmd[3] : (endOfLine = true, ""),                         \
             (!endOfLine && atCmd[4]) ? atCmd[4] : (endOfLine = true, ""),                         \
             (!endOfLine && atCmd[5]) ? atCmd[5] : (endOfLine = true, ""),                         \
             (!endOfLine && atCmd[6]) ? atCmd[6] : (endOfLine = true, ""),                         \
             (!endOfLine && atCmd[7]) ? atCmd[7] : (endOfLine = true, ""),                         \
             (!endOfLine && atCmd[8]) ? atCmd[8] : (endOfLine = true, ""),                         \
             (!endOfLine && atCmd[9]) ? atCmd[9] : (endOfLine = true, ""),                         \
             (!endOfLine && atCmd[10]) ? atCmd[10] : (endOfLine = true, ""),                       \
             (!endOfLine && atCmd[11]) ? atCmd[11] : (endOfLine = true, ""),                       \
             (!endOfLine && atCmd[12]) ? atCmd[12] : (endOfLine = true, ""),                       \
             (!endOfLine && atCmd[13]) ? atCmd[13] : (endOfLine = true, ""),                       \
             (!endOfLine && atCmd[14]) ? atCmd[14] : (endOfLine = true, ""),                       \
             (!endOfLine && atCmd[15]) ? atCmd[15] : (endOfLine = true, ""),                       \
             (!endOfLine && atCmd[16]) ? atCmd[16] : (endOfLine = true, ""),                       \
             (!endOfLine && atCmd[17]) ? atCmd[17] : (endOfLine = true, ""),                       \
             (!endOfLine && atCmd[18]) ? atCmd[18] : (endOfLine = true, ""),                       \
             (!endOfLine && atCmd[19]) ? atCmd[19] : (endOfLine = true, ""),                       \
             (!endOfLine && atCmd[20]) ? atCmd[20] : (endOfLine = true, ""),                       \
             (!endOfLine && atCmd[21]) ? atCmd[21] : (endOfLine = true, ""),                       \
             (!endOfLine && atCmd[22]) ? atCmd[22] : (endOfLine = true, ""),                       \
             (!endOfLine && atCmd[23]) ? atCmd[23] : (endOfLine = true, ""),                       \
             (!endOfLine && atCmd[24]) ? atCmd[24] : (endOfLine = true, ""),                       \
             (!endOfLine && atCmd[25]) ? atCmd[25] : (endOfLine = true, ""),                       \
             (!endOfLine && atCmd[26]) ? atCmd[26] : (endOfLine = true, ""),                       \
             (!endOfLine && atCmd[27]) ? atCmd[27] : (endOfLine = true, ""),                       \
             (!endOfLine && atCmd[28]) ? atCmd[28] : (endOfLine = true, ""),                       \
             (!endOfLine && atCmd[29]) ? atCmd[29] : (endOfLine = true, ""),                       \
             (!endOfLine && atCmd[30]) ? atCmd[30] : (endOfLine = true, ""),                       \
             (!endOfLine && atCmd[31]) ? atCmd[31] : (endOfLine = true, ""),                       \
             (!endOfLine && atCmd[32]) ? atCmd[32] : (endOfLine = true, ""),                       \
             (!endOfLine && atCmd[33]) ? atCmd[33] : (endOfLine = true, ""),                       \
             (!endOfLine && atCmd[34]) ? atCmd[34] : (endOfLine = true, ""),                       \
             (!endOfLine && atCmd[35]) ? atCmd[35] : (endOfLine = true, ""),                       \
             (!endOfLine && atCmd[36]) ? atCmd[36] : (endOfLine = true, ""),                       \
             (!endOfLine && atCmd[37]) ? atCmd[37] : (endOfLine = true, ""),                       \
             (!endOfLine && atCmd[38]) ? atCmd[38] : (endOfLine = true, ""),                       \
             (!endOfLine && atCmd[39]) ? atCmd[39] : (endOfLine = true, ""),                       \
             (!endOfLine && atCmd[40]) ? atCmd[40] : (endOfLine = true, ""),                       \
             (!endOfLine && atCmd[41]) ? atCmd[41] : (endOfLine = true, ""),                       \
             (!endOfLine && atCmd[42]) ? atCmd[42] : (endOfLine = true, ""),                       \
             (!endOfLine && atCmd[43]) ? atCmd[43] : (endOfLine = true, ""),                       \
             (!endOfLine && atCmd[44]) ? atCmd[44] : (endOfLine = true, ""),                       \
             (!endOfLine && atCmd[45]) ? atCmd[45] : (endOfLine = true, ""),                       \
             (!endOfLine && atCmd[46]) ? atCmd[46] : (endOfLine = true, ""),                       \
             (!endOfLine && atCmd[47]) ? atCmd[47] : (endOfLine = true, ""),                       \
             (!endOfLine && atCmd[48]) ? atCmd[48] : (endOfLine = true, ""),                       \
             (!endOfLine && atCmd[49]) ? atCmd[49] : (endOfLine = true, ""),                       \
             (!endOfLine && atCmd[50]) ? atCmd[50] : (endOfLine = true, ""),                       \
             (!endOfLine && atCmd[51]) ? atCmd[51] : (endOfLine = true, ""),                       \
             (!endOfLine && atCmd[52]) ? atCmd[52] : (endOfLine = true, ""),                       \
             (!endOfLine && atCmd[53]) ? atCmd[53] : (endOfLine = true, ""),                       \
             (!endOfLine && atCmd[54]) ? atCmd[54] : (endOfLine = true, ""),                       \
             (!endOfLine && atCmd[55]) ? atCmd[55] : (endOfLine = true, ""),                       \
             (!endOfLine && atCmd[56]) ? atCmd[56] : (endOfLine = true, ""),                       \
             (!endOfLine && atCmd[57]) ? atCmd[57] : (endOfLine = true, ""),                       \
             (!endOfLine && atCmd[58]) ? atCmd[58] : (endOfLine = true, ""),                       \
             (!endOfLine && atCmd[59]) ? atCmd[59] : (endOfLine = true, ""),                       \
             (!endOfLine && atCmd[60]) ? atCmd[60] : (endOfLine = true, ""),                       \
             (!endOfLine && atCmd[61]) ? atCmd[61] : (endOfLine = true, ""),                       \
             (!endOfLine && atCmd[62]) ? atCmd[62] : (endOfLine = true, ""));                      \
    for(int i = 0; i < WALTER_MODEM_COMMAND_MAX_ELEMS; ++i) {                                      \
      if(atCmd[i] == NULL) {                                                                       \
        break;                                                                                     \
      }                                                                                            \
      /* PAGER PATCH: 1.17 -- the AT command element(s) about to be written, for the flight     */ \
      /* recorder; the trailing "\r\n"/"\n" below is not traced separately (see the hook's own  */ \
      /* doc comment: one call with the command string is enough). Instrumentation only.        */ \
      if(s_pagerTraceHook) {                                                                       \
        s_pagerTraceHook('T', (const uint8_t*) atCmd[i], strlen(atCmd[i]));                        \
      }                                                                                            \
      uart_write_bytes(_uartNo, atCmd[i], strlen(atCmd[i]));                                       \
    }                                                                                              \
    if(type == WALTER_MODEM_CMD_TYPE_DATA_TX_WAIT)                                                 \
      uart_write_bytes(_uartNo, "\n", 1);                                                          \
    else                                                                                           \
      uart_write_bytes(_uartNo, "\r\n", 2);                                                        \
  }
#endif

#define CONSTANT(name, type, value) inline static constexpr type name = value;
#endif