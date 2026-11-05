/* Regression tests for the UART frame decoder's resynchronisation.
 *
 * Why this file belongs in tests/ and not in the board bench:
 * the defect is in src/core/can_uart.c, so it must fail in the core suite the
 * moment somebody re-introduces a "collect until the claimed length" decoder.
 * It needs no hardware and no board: the whole test is a byte stream.
 *
 * What it pins down:
 *   1. a truncated frame must never swallow the frame that follows it;
 *   2. random noise of arbitrary length must be followed by recovery, not by a
 *      permanently deaf decoder;
 *   3. a payload that happens to contain A5 5A must NOT be treated as a new
 *      sync word -- this is the risk the naive "reset on any A5 5A" fix takes.
 */

#include <stdio.h>
#include <string.h>

#include "ota/can_uart.h"
#include "test_util.h"

#define BUFSZ 160u

static uint32_t build(uint8_t id_low, uint8_t dlc, uint8_t seed,
                      uint8_t *out, uint32_t max)
{
    can_frame_t f;
    uint32_t i;

    memset(&f, 0, sizeof f);
    f.id = 0x7E0u + id_low;
    f.dlc = dlc;
    for (i = 0u; i < dlc; i++) {
        f.data[i] = (uint8_t)(seed + (uint8_t)(i * 17u));
    }
    return can_uart_encode(&f, out, max);
}

/* Feed a byte stream, count delivered frames and remember the first id. */
static int feed_stream(const uint8_t *raw, uint32_t len, int *first_id)
{
    can_uart_decoder_t d;
    can_frame_t f;
    uint32_t i;
    int count = 0;

    if (first_id) {
        *first_id = -1;
    }
    can_uart_decoder_init(&d);
    for (i = 0u; i < len; i++) {
        if (can_uart_decoder_feed(&d, raw[i], &f) == 1) {
            if (count == 0 && first_id) {
                *first_id = (int)f.id;
            }
            count++;
        }
    }
    return count;
}

/* 1. Every possible truncation of the first frame, followed by two valid ones. */
static void test_truncation_never_swallows_the_next_frame(void)
{
    uint8_t good[CAN_UART_MAX_FRAME];
    uint8_t stream[BUFSZ];
    uint32_t dlc, n, cut, off, m;
    int cases = 0, lost = 0;

    for (dlc = 1u; dlc <= 8u; dlc++) {
        n = build(0u, (uint8_t)dlc, 0x10u, good, sizeof good);
        for (cut = 1u; cut + 1u < n; cut++) {
            off = cut;
            m = build(1u, 3u, 0x40u, stream + off, sizeof stream - off);
            off += m;
            m = build(2u, 5u, 0x60u, stream + off, sizeof stream - off);
            off += m;
            memcpy(stream, good, cut);
            cases++;
            {
                int first_id = -1;
                int count = feed_stream(stream, off, &first_id);
                if (count < 2 || first_id != 0x7E1) {
                    lost++;
                }
            }
        }
    }
    printf("  truncation sweep: %d cut points, %d swallowed\n", cases, lost);
    TEST_ASSERT(cases > 0);
    TEST_ASSERT(lost == 0);
}

/* 2. Random noise, then two valid frames: the decoder must recover. */
static void test_random_noise_then_recovery(void)
{
    uint8_t stream[BUFSZ];
    uint32_t state = 0x12345678u;
    int seed, deaf = 0;

    for (seed = 0; seed < 200; seed++) {
        uint32_t off = 0;
        uint32_t noise_len, i, m;
        int first_id = -1, count;

        for (i = 0u; i < 8u; i++) {
            state = state * 1103515245u + 12345u;
            noise_len = (state >> 16) % 40u;
        }
        for (i = 0u; i < noise_len && off < 60u; i++) {
            state = state * 1103515245u + 12345u;
            stream[off++] = (uint8_t)(state >> 24);
        }
        m = build(1u, 4u, 0x22u, stream + off, sizeof stream - off);
        off += m;
        m = build(2u, 6u, 0x33u, stream + off, sizeof stream - off);
        off += m;

        count = feed_stream(stream, off, &first_id);
        if (count < 1 || first_id != 0x7E1) {
            deaf++;
        }
    }
    printf("  random noise: 200 streams, %d never recovered\n", deaf);
    TEST_ASSERT(deaf == 0);
}

/* 3. A5 5A inside the payload is data, not a sync word. */
static void test_sync_bytes_inside_payload_are_data(void)
{
    can_frame_t f, out;
    can_uart_decoder_t d;
    uint8_t raw[CAN_UART_MAX_FRAME];
    uint32_t n, i;
    int got = 0;

    memset(&f, 0, sizeof f);
    f.id = 0x7E0u;
    f.dlc = 8u;
    f.data[0] = 0xA5u;
    f.data[1] = 0x5Au;
    f.data[2] = 0xA5u;
    f.data[3] = 0x5Au;
    f.data[4] = 0x01u;
    f.data[5] = 0x02u;
    f.data[6] = 0x03u;
    f.data[7] = 0x04u;

    n = can_uart_encode(&f, raw, sizeof raw);
    TEST_ASSERT(n > 0u);

    can_uart_decoder_init(&d);
    for (i = 0u; i < n; i++) {
        if (can_uart_decoder_feed(&d, raw[i], &out) == 1) {
            got++;
            TEST_ASSERT(out.id == f.id);
            TEST_ASSERT(out.dlc == f.dlc);
            TEST_ASSERT(memcmp(out.data, f.data, f.dlc) == 0);
        }
    }
    printf("  payload containing A5 5A: %d frame(s) delivered\n", got);
    TEST_ASSERT(got == 1);
}

/* 4. A clean stream stays clean: no frames invented, none dropped. */
static void test_clean_stream_is_untouched(void)
{
    uint8_t stream[BUFSZ];
    uint32_t off = 0, m;
    int count, i;

    for (i = 0; i < 10; i++) {
        m = build((uint8_t)i, (uint8_t)(1 + (i % 8)), (uint8_t)(i * 9),
                  stream + off, sizeof stream - off);
        off += m;
    }
    count = feed_stream(stream, off, NULL);
    printf("  clean stream: %d frames encoded, %d decoded\n", 10, count);
    TEST_ASSERT(count == 10);
}

void run_uart_fuzz_tests(void)
{
    printf("uart fuzz / resync:\n");
    test_truncation_never_swallows_the_next_frame();
    test_random_noise_then_recovery();
    test_sync_bytes_inside_payload_are_data();
    test_clean_stream_is_untouched();
}
