/* Response-builder bounds checks for uds_handle().
 *
 * The assertions that reach the shared counter are deliberately few: the
 * sweep below performs thousands of internal probes and reports them as a
 * single "violations == 0" assertion, so the suite-wide check count stays
 * comparable with earlier runs. Set UDS_BOUNDS_VERBOSE to see every probe.
 */
#include "bench.h"
#include "test_util.h"

#include <stdio.h>
#include <string.h>

#define GUARD 64u
#define PROBE_MAX 600u
#define SWEEP_ITERS 4000u

static bench_t UB;
static uint8_t raw[GUARD + PROBE_MAX + GUARD];

static unsigned probe_checks;
static unsigned violations;
static uint32_t rng_state;

static uint32_t rnd(void)
{
    uint32_t x = rng_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    rng_state = x;
    return x;
}

static void violation(const char *tag, const char *what, unsigned a, unsigned b)
{
    violations++;
    printf("  FAIL uds bounds [%s] %s (%u, %u)\n", tag, what, a, b);
}

static int nrc_is_defined(uint8_t code)
{
    switch (code) {
    case 0x10u: case 0x11u: case 0x12u: case 0x13u: case 0x14u:
    case 0x21u: case 0x22u: case 0x24u: case 0x25u: case 0x26u:
    case 0x31u: case 0x33u: case 0x34u: case 0x35u: case 0x36u:
    case 0x37u: case 0x70u: case 0x71u: case 0x72u: case 0x73u:
    case 0x78u: case 0x7Eu: case 0x7Fu:
        return 1;
    default:
        return 0;
    }
}

/* One guarded call plus the three invariants. */
static uint16_t probe(const uint8_t *req, uint16_t req_len, uint16_t resp_max,
                      const char *tag)
{
    uint16_t n;
    uint16_t i;

    memset(raw, 0xA5, sizeof raw);
    n = uds_handle(&UB.server, req, req_len, &raw[GUARD], resp_max);
    probe_checks++;

    if (n > resp_max) {
        violation(tag, "returned length above resp_max", (unsigned)n, (unsigned)resp_max);
        n = resp_max;
    }
    for (i = 0u; i < GUARD; i++) {
        if (raw[i] != 0xA5u) {
            violation(tag, "write before the response buffer", (unsigned)(GUARD - i),
                      (unsigned)resp_max);
            break;
        }
    }
    for (i = GUARD + resp_max; i < sizeof raw; i++) {
        if (raw[i] != 0xA5u) {
            violation(tag, "write past resp_max", (unsigned)(i - GUARD - resp_max),
                      (unsigned)resp_max);
            break;
        }
    }
    if (n >= 3u && raw[GUARD] == 0x7Fu) {
        if (!nrc_is_defined(raw[GUARD + 2u])) {
            violation(tag, "undefined NRC", (unsigned)raw[GUARD + 2u], 0u);
        }
        if (raw[GUARD + 1u] != req[0]) {
            violation(tag, "negative response echoes the wrong SID",
                      (unsigned)raw[GUARD + 1u], (unsigned)req[0]);
        }
    }
    return n;
}

static void response_too_long_cases(void)
{
    static const uint8_t vin1[] = {0x22u, 0xF1u, 0x90u};
    static const uint8_t vin3[] = {0x22u, 0xF1u, 0x90u, 0xF1u, 0x90u, 0xF1u, 0x90u};
    static const uint8_t status[] = {0x22u, 0x02u, 0x00u};

    /* one VIN DID = 1 + 2 + 17 = 20 bytes: exactly fits, and one byte less
     * must be reported as responseTooLong instead of a truncated payload */
    TEST_ASSERT(probe(vin1, 3u, 20u, "vin/20") == 20u && raw[GUARD] == 0x62u);
    TEST_ASSERT(probe(vin1, 3u, 19u, "vin/19") == 3u && raw[GUARD + 2u] == 0x14u);

    /* 0x0200 carries 40 bytes, so 42 is one short and 43 is exactly enough */
    TEST_ASSERT(probe(status, 3u, 42u, "status/42") == 3u && raw[GUARD + 2u] == 0x14u);
    TEST_ASSERT(probe(status, 3u, 43u, "status/43") == 43u && raw[GUARD] == 0x62u);

    /* two DIDs fit in 39 bytes, three do not fit in 42 */
    TEST_ASSERT(probe(vin3, 5u, 39u, "2vin/39") == 39u && raw[GUARD] == 0x62u);
    TEST_ASSERT(probe(vin3, 7u, 42u, "3vin/42") == 3u && raw[GUARD + 2u] == 0x14u);
}

/* A storage failure must still be generalProgrammingFailure, not 0x14:
 * the two causes have to stay distinguishable. */
static void storage_failure_still_0x72(void)
{
    static const uint8_t status[] = {0x22u, 0x02u, 0x00u};

    UB.eeprom.read_fail_after = 0;
    (void)probe(status, 3u, 43u, "status/43/eeprom-fail");
    TEST_ASSERT(raw[GUARD] == 0x7Fu && raw[GUARD + 2u] == 0x72u);
    UB.eeprom.read_fail_after = -1;
}

static void small_buffer_sweep(void)
{
    static const uint8_t sids[] = {0x10u, 0x11u, 0x22u, 0x27u, 0x31u,
                                   0x34u, 0x36u, 0x37u, 0x3Eu, 0x99u};
    uint8_t req[16];
    uint16_t max;
    unsigned i;

    for (i = 0u; i < sizeof sids; i++) {
        memset(req, 0, sizeof req);
        req[0] = sids[i];
        for (max = 3u; max <= 8u; max++) {
            char tag[24];
            snprintf(tag, sizeof tag, "sid%02X/max%u", (unsigned)sids[i], (unsigned)max);
            (void)probe(req, 2u, max, tag);
        }
    }
}

static void random_sweep(void)
{
    static const uint8_t did_pool[][2] = {
        {0xF1u, 0x90u}, {0xF1u, 0x87u}, {0xF1u, 0x95u}, {0xF1u, 0x86u},
        {0x02u, 0x00u}, {0x12u, 0x34u}, {0xFFu, 0xFFu}, {0x00u, 0x00u}
    };
    static const uint8_t sid_pool[] = {0x10u, 0x11u, 0x22u, 0x27u, 0x31u,
                                       0x34u, 0x36u, 0x37u, 0x3Eu};
    uint8_t req[80];
    unsigned iter;

    rng_state = 0x12345678u;
    for (iter = 0u; iter < SWEEP_ITERS; iter++) {
        uint16_t resp_max = (uint16_t)(3u + (rnd() % 120u));
        uint8_t sid = sid_pool[rnd() % (sizeof sid_pool)];
        uint16_t len = (uint16_t)(1u + (rnd() % 40u));
        uint16_t i;

        memset(req, 0, sizeof req);
        req[0] = sid;
        if (sid == 0x22u) {
            uint16_t dids = (uint16_t)(1u + (rnd() % 12u));
            if ((uint16_t)(dids * 2u + 1u) > len) {
                len = (uint16_t)(dids * 2u + 1u);
            }
            for (i = 0u; i < dids; i++) {
                if ((rnd() & 3u) == 0u) {
                    req[1u + i * 2u] = (uint8_t)rnd();
                    req[2u + i * 2u] = (uint8_t)rnd();
                } else {
                    uint8_t k = (uint8_t)(rnd() % (sizeof did_pool / 2u));
                    req[1u + i * 2u] = did_pool[k][0];
                    req[2u + i * 2u] = did_pool[k][1];
                }
            }
        } else {
            for (i = 1u; i < len; i++) {
                req[i] = (uint8_t)rnd();
            }
        }
        if ((iter % 200u) == 0u) {
            /* keep the security lockout from masking every later 0x27 path */
            UB.server.key_failures = 0u;
            UB.server.lockout_active = 0u;
            UB.server.unlocked = 0u;
            UB.server.session = UDS_SESSION_DEFAULT;
        }
        (void)probe(req, len, resp_max, "rand");
    }
}

void run_uds_bounds_tests(void)
{
    printf("[uds bounds]\n");
    bench_init(&UB);
    probe_checks = 0u;
    violations = 0u;

    response_too_long_cases();
    storage_failure_still_0x72();
    small_buffer_sweep();
    random_sweep();

    printf("  %u guarded calls, %u violations\n", probe_checks, violations);
    TEST_ASSERT(violations == 0u);
    TEST_ASSERT(probe_checks > 4000u);
}
