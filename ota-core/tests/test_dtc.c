#include "bench.h"
#include "test_util.h"

#include <string.h>

static bench_t B;
static uint8_t req[32];
static uint8_t resp[BENCH_RESP_MAX];
static uint8_t small[24];
static uint16_t rl;

static int call(uint16_t n)
{
    rl = 0u;
    return bench_request(&B, req, n, resp, &rl);
}

static void wrong_key_once(void)
{
    req[0] = 0x27;
    req[1] = 0x01;
    (void)call(2);
    req[1] = 0x02;
    t_put_be32(&req[2], t_get_be32(&resp[2]) + 1u);
    (void)call(6);
}

static int unlock(void)
{
    uint32_t seed;

    req[0] = 0x10;
    req[1] = 0x02;
    if (call(2) != 1) return 0;
    req[0] = 0x27;
    req[1] = 0x01;
    if (call(2) != 1) return 0;
    seed = t_get_be32(&resp[2]);
    req[1] = 0x02;
    t_put_be32(&req[2], uds_calc_key(seed));
    if (call(6) != 1) return 0;
    return resp[0] == 0x67u && resp[1] == 0x02u;
}

/* 0x19 01 FF -> stored DTC count, -1 when the answer is malformed */
static int dtc_count(void)
{
    req[0] = 0x19;
    req[1] = 0x01;
    req[2] = 0xFF;
    if (call(3) != 1 || rl != 6u || resp[0] != 0x59u || resp[1] != 0x01u) {
        return -1;
    }
    return (int)(((uint16_t)resp[4] << 8) | (uint16_t)resp[5]);
}

/* offset of a 3-byte DTC inside a 0x19 02 answer, -1 when absent */
static int dtc_index(uint32_t code)
{
    uint16_t n;

    req[0] = 0x19;
    req[1] = 0x02;
    req[2] = 0xFF;
    if (call(3) != 1 || resp[0] != 0x59u) {
        return -1;
    }
    for (n = 3u; (uint16_t)(n + 4u) <= rl; n = (uint16_t)(n + 4u)) {
        uint32_t c = ((uint32_t)resp[n + 1u] << 16)
                   | ((uint32_t)resp[n + 2u] << 8) | (uint32_t)resp[n + 3u];
        if (c == code) {
            return (int)n;
        }
    }
    return -1;
}

/* Builds the 0x14 request from scratch: request bytes are shared state, so a
   helper that overwrote only part of req[] silently sent a 0x19 last time. */
static void clear_req(uint32_t group)
{
    memset(req, 0, sizeof req);
    req[0] = 0x14;
    req[1] = (uint8_t)(group >> 16);
    req[2] = (uint8_t)(group >> 8);
    req[3] = (uint8_t)group;
}

static uint16_t start_download(uint32_t size)
{
    memset(req, 0, sizeof req);
    req[0] = 0x34;
    req[1] = 0x00;
    req[2] = 0x44;
    t_put_be32(&req[3], APP_BASE_ADDR);
    t_put_be32(&req[7], size);
    return (uint16_t)call(11);
}

void run_dtc_tests(void)
{
    int i;

    bench_init(&B);
    printf("[dtc]\n");

    bench_init(&B);

    /* An empty table is a valid answer, not an error. */
    req[0] = 0x19;
    req[1] = 0x02;
    req[2] = 0xFF;
    TEST_ASSERT(call(3) == 1);
    TEST_ASSERT(rl == 3u);
    TEST_ASSERT(resp[0] == 0x59u && resp[1] == 0x02u);
    TEST_ASSERT(resp[2] == UDS_DTC_STATUS_AVAILABILITY);
    TEST_ASSERT(resp[2] == 0x2Bu);
    TEST_ASSERT(dtc_count() == 0);

    /* A recorded DTC comes back as status byte + 3 code bytes. */
    uds_dtc_record(&B.server, UDS_DTC_EXT_FLASH);
    TEST_ASSERT(dtc_count() == 1);
    TEST_ASSERT(dtc_index(UDS_DTC_EXT_FLASH) == 3);
    TEST_ASSERT(resp[3] == UDS_DTC_STATUS_AVAILABILITY);
    TEST_ASSERT(resp[4] == 0xC1u && resp[5] == 0x01u && resp[6] == 0x00u);

    /* Re-recording the same code must not add a second row. */
    uds_dtc_record(&B.server, UDS_DTC_EXT_FLASH);
    TEST_ASSERT(dtc_count() == 1);
    TEST_ASSERT(B.server.dtc_count == 1u);

    /* The status mask filters: 0x00 matches nothing, 0x08 matches confirmed. */
    req[0] = 0x19;
    req[1] = 0x02;
    req[2] = 0x00;
    TEST_ASSERT(call(3) == 1 && rl == 3u);
    req[2] = UDS_DTC_STATUS_CONFIRMED;
    TEST_ASSERT(call(3) == 1 && rl == 7u);

    /* 0x14 FFFFFF clears, and clearing an empty table is still positive. */
    clear_req(0xFFFFFFu);
    TEST_ASSERT(call(4) == 1);
    TEST_ASSERT(rl == 1u && resp[0] == 0x54u);
    TEST_ASSERT(dtc_count() == 0);
    clear_req(0xFFFFFFu);
    TEST_ASSERT(call(4) == 1 && rl == 1u && resp[0] == 0x54u);

    /* Only "all groups" exists, and the request length is exact. */
    clear_req(0x000001u);
    TEST_ASSERT(call(4) == 1 && rl == 3u && resp[2] == 0x31u);
    clear_req(0xFFFFFFu);
    TEST_ASSERT(call(3) == 1 && rl == 3u && resp[2] == 0x13u);

    /* Unsupported sub function, truncated 0x19. */
    req[0] = 0x19;
    req[1] = 0x0A;
    req[2] = 0xFF;
    TEST_ASSERT(call(3) == 1 && rl == 3u && resp[2] == 0x12u);
    TEST_ASSERT(call(2) == 1 && rl == 3u && resp[2] == 0x13u);

    /* More distinct codes than the table holds: keep 8, count the rest. */
    bench_init(&B);
    for (i = 0; i < 8; i++) {
        uds_dtc_record(&B.server, 0xC20000u + (uint32_t)i);
    }
    uds_dtc_record(&B.server, 0xC30000u);
    TEST_ASSERT(B.server.dtc_count == UDS_DTC_MAX);
    TEST_ASSERT(B.server.dtc_dropped == 1u);
    TEST_ASSERT(dtc_count() == (int)UDS_DTC_MAX);
    uds_dtc_record(&B.server, 0xC20000u); /* merge still works when full */
    TEST_ASSERT(B.server.dtc_count == UDS_DTC_MAX);
    TEST_ASSERT(B.server.dtc_dropped == 1u);

    /* A full table does not fit 24 bytes: answer 0x14, never truncate. */
    memset(small, 0x5A, sizeof small);
    req[0] = 0x19;
    req[1] = 0x02;
    req[2] = 0xFF;
    TEST_ASSERT(uds_handle(&B.server, req, 3u, small, sizeof small) == 3u);
    TEST_ASSERT(small[0] == 0x7Fu && small[1] == 0x19u && small[2] == 0x14u);
    TEST_ASSERT(small[3] == 0x5Au && small[23] == 0x5Au);

    /* A buffer of exactly 3 + 4 * count is enough. */
    {
        uint8_t exact[3u + 4u * UDS_DTC_MAX];
        memset(exact, 0x5A, sizeof exact);
        TEST_ASSERT(uds_handle(&B.server, req, 3u, exact, sizeof exact) ==
                    (uint16_t)sizeof exact);
        TEST_ASSERT(exact[3] == UDS_DTC_STATUS_AVAILABILITY);
        TEST_ASSERT(exact[34] == 0x07u);
    }

    /* Suppressing the positive response must not change the table. */
    req[1] = 0x82;
    TEST_ASSERT(uds_handle(&B.server, req, 3u, resp, BENCH_RESP_MAX) == 0u);
    TEST_ASSERT(B.server.dtc_count == UDS_DTC_MAX);

    /* RAM only: a reboot (power cycle) starts with an empty table. */
    bench_reboot(&B);
    TEST_ASSERT(dtc_count() == 0);

    /* 3 wrong keys -> lockout, and that failure leaves a readable DTC. */
    bench_init(&B);
    req[0] = 0x10;
    req[1] = 0x02;
    TEST_ASSERT(call(2) == 1);
    wrong_key_once();
    wrong_key_once();
    wrong_key_once();
    TEST_ASSERT(resp[0] == 0x7Fu && resp[2] == 0x36u);
    TEST_ASSERT(dtc_index(UDS_DTC_SECURITY) >= 0);
    TEST_ASSERT(dtc_count() == 1);

    /* A metadata read failure during 0x34 leaves the METADATA DTC. */
    bench_init(&B);
    TEST_ASSERT(unlock());
    B.eeprom.read_fail_after = 0;
    TEST_ASSERT(start_download(IMAGE_HEADER_SIZE + 8u) == 1);
    TEST_ASSERT(rl == 3u && resp[2] == 0x72u);
    B.eeprom.read_fail_after = -1;
    TEST_ASSERT(dtc_index(UDS_DTC_METADATA) >= 0);

    /* A failed 0x36 write leaves the EXT_FLASH DTC. */
    bench_init(&B);
    TEST_ASSERT(unlock());
    TEST_ASSERT(start_download(IMAGE_HEADER_SIZE + 8u) == 1);
    TEST_ASSERT(resp[0] == 0x74u);
    B.fault.powered_off = 1;
    req[0] = 0x36;
    req[1] = 0x01;
    memset(&req[2], 0xAA, 8u);
    TEST_ASSERT(call(10) == 1);
    TEST_ASSERT(rl == 3u && resp[2] == 0x72u);
    TEST_ASSERT(dtc_index(UDS_DTC_EXT_FLASH) >= 0);

    /* A failing routine 0001 on a READY package leaves the PACKAGE DTC. */
    bench_init(&B);
    TEST_ASSERT(unlock());
    {
        metadata_t m;
        memset(&m, 0, sizeof m);
        m.state = UPDATE_READY;
        m.active_version = 1u;
        m.active_size = 16u;
        m.active_crc = 0x55667788u;
        m.min_version = 1u;
        m.pending_version = 2u;
        m.pending_size = 64u;
        m.pending_crc = 0x11223344u;
        TEST_ASSERT(metadata_save(&B.meta, &m) == 0);
    }
    req[0] = 0x31;
    req[1] = 0x01;
    req[2] = 0x00;
    req[3] = 0x01;
    TEST_ASSERT(call(4) == 1);
    TEST_ASSERT(rl == 3u && resp[2] == 0x72u);
    TEST_ASSERT(dtc_index(UDS_DTC_PACKAGE) >= 0);

    /* Clearing after a real failure makes 0x19 empty again. */
    clear_req(0xFFFFFFu);
    TEST_ASSERT(call(4) == 1 && rl == 1u);
    TEST_ASSERT(dtc_count() == 0);
}
