/* ISO-TP / UDS input-boundary probe.
 *
 * Purpose: exercise the edges that tests/test_isotp.c and
 * tests/test_protocol_edges.c do not touch, and report the *measured*
 * behaviour instead of a code-reading opinion.
 *
 * Read-only consumer: it includes the product sources but never modifies
 * them.  Build with the host port (src/port/host) and tests/bench.c.
 *
 * Two kinds of checks:
 *   TEST_ASSERT - a contract the implementation must satisfy.
 *   NOTE        - current behaviour recorded for the report; printing a
 *                 NOTE means the measured behaviour differs from the
 *                 ISO 14229 / ISO 15765-2 wording, but it is not a crash
 *                 and does not change the pass/fail verdict.
 */
#include "bench.h"
#include "test_util.h"

#include <stdio.h>
#include <string.h>

static int g_notes = 0;

#define NOTE(cond, msg)                                                       \
    do {                                                                      \
        g_checks++;                                                           \
        if (!(cond)) {                                                        \
            g_notes++;                                                        \
            printf("  NOTE %s:%d  %s\n", __FILE__, __LINE__, (msg));          \
        }                                                                     \
    } while (0)

/* ------------------------------------------------------------------ */
/* minimal single-link wire tap                                        */
/* ------------------------------------------------------------------ */
static struct {
    isotp_link_t link;
    ota_port_t   port;
    uint32_t     now;
    uint32_t     sent;
    can_frame_t  frame[64];
    uint32_t     nframes;
} W;

static int w_send(void *user, const can_frame_t *f)
{
    (void)user;
    if (W.nframes < 64u) {
        W.frame[W.nframes] = *f;
        W.nframes++;
    }
    W.sent++;
    return 0;
}

static uint32_t w_now(void *user)
{
    (void)user;
    return W.now;
}

static void iso_fresh(void)
{
    memset(&W, 0, sizeof W);
    W.port.can_send = w_send;
    W.port.now_ms  = w_now;
    W.port.user    = &W;
    isotp_init(&W.link, &W.port, 0x7E0u, 0x7E8u);
}

static void feed(const uint8_t *d, uint8_t dlc)
{
    can_frame_t f;
    memset(&f, 0, sizeof f);
    f.id  = 0x7E8u;
    f.dlc = dlc;
    memcpy(f.data, d, dlc);
    isotp_on_frame(&W.link, &f);
}

/* ------------------------------------------------------------------ */
/* A. transmit length boundaries                                       */
/* ------------------------------------------------------------------ */
static void send_length_edges(void)
{
    uint8_t buf[ISOTP_MAX_PAYLOAD];
    memset(buf, 0x5Au, sizeof buf);

    puts("[A] isotp_send length boundaries");

    /* zero length must not touch the bus and must not start a transfer */
    iso_fresh();
    TEST_ASSERT(isotp_send(&W.link, buf, 0u) == ISOTP_ERR_LENGTH);
    TEST_ASSERT(W.nframes == 0u);
    TEST_ASSERT(W.link.tx_state == ISOTP_IDLE);

    /* 7 bytes is still a single frame: PCI=0x07, dlc=8 */
    iso_fresh();
    TEST_ASSERT(isotp_send(&W.link, buf, 7u) == ISOTP_OK);
    TEST_ASSERT(W.nframes == 1u);
    TEST_ASSERT(W.frame[0].dlc == 8u);
    TEST_ASSERT(W.frame[0].data[0] == 0x07u);
    TEST_ASSERT(W.link.tx_state == ISOTP_IDLE);

    /* 8 bytes must switch to a first frame: 0x10, length 0x0008 */
    iso_fresh();
    TEST_ASSERT(isotp_send(&W.link, buf, 8u) == ISOTP_OK);
    TEST_ASSERT(W.nframes == 1u);
    TEST_ASSERT(W.frame[0].data[0] == 0x10u);
    TEST_ASSERT(W.frame[0].data[1] == 0x08u);
    TEST_ASSERT(W.link.tx_state == ISOTP_WAIT_FC);

    /* 2048 = ISOTP_MAX_PAYLOAD: 12-bit length field, high nibble 0x8 */
    iso_fresh();
    TEST_ASSERT(isotp_send(&W.link, buf, ISOTP_MAX_PAYLOAD) == ISOTP_OK);
    TEST_ASSERT(W.frame[0].data[0] == 0x18u);
    TEST_ASSERT(W.frame[0].data[1] == 0x00u);

    /* one past the buffer, the 12-bit limit, and a full uint16_t */
    iso_fresh();
    TEST_ASSERT(isotp_send(&W.link, buf, (uint16_t)(ISOTP_MAX_PAYLOAD + 1u)) == ISOTP_ERR_LENGTH);
    TEST_ASSERT(isotp_send(&W.link, buf, 4095u) == ISOTP_ERR_LENGTH);
    TEST_ASSERT(isotp_send(&W.link, buf, 4096u) == ISOTP_ERR_LENGTH);
    TEST_ASSERT(isotp_send(&W.link, buf, 65535u) == ISOTP_ERR_LENGTH);
    TEST_ASSERT(W.nframes == 0u);

    /* NULL arguments */
    iso_fresh();
    TEST_ASSERT(isotp_send(&W.link, 0, 8u) == ISOTP_ERR_LENGTH);
    TEST_ASSERT(isotp_send(0, buf, 8u) == ISOTP_ERR_LENGTH);

    /* a rejected length leaves tx_error untouched (it is only set by abort_tx) */
    iso_fresh();
    W.link.tx_error = 0;
    (void)isotp_send(&W.link, buf, 0u);
    NOTE(W.link.tx_error != 0, "rejected length does not record tx_error; caller must use the return value");
}

/* ------------------------------------------------------------------ */
/* B. first-frame receive boundaries                                   */
/* ------------------------------------------------------------------ */
static void rx_first_frame_edges(void)
{
    uint8_t f[8];

    puts("[B] isotp_on_frame first frame");

    /* length 7 is illegal for a first frame (would be a single frame) */
    iso_fresh();
    f[0] = 0x10u; f[1] = 0x07u; f[2] = 0xAAu;
    feed(f, 3u);
    TEST_ASSERT(W.link.rx_state == ISOTP_IDLE);
    TEST_ASSERT(W.nframes == 0u);           /* no flow control */

    /* exact minimum: length 8, dlc 8 -> 6 bytes stored */
    iso_fresh();
    f[0] = 0x10u; f[1] = 0x08u;
    memset(&f[2], 0x11, 6u);
    feed(f, 8u);
    TEST_ASSERT(W.link.rx_state == ISOTP_WAIT_CF);
    TEST_ASSERT(W.link.rx_len == 8u);
    TEST_ASSERT(W.link.rx_got == 6u);
    TEST_ASSERT(W.nframes == 1u);
    TEST_ASSERT(W.frame[0].data[0] == 0x30u);

    /* one past ISOTP_MAX_PAYLOAD -> overflow FC (0x32), link stays idle */
    iso_fresh();
    f[0] = 0x18u; f[1] = 0x01u;
    feed(f, 8u);
    TEST_ASSERT(W.link.rx_error == ISOTP_ERR_OVERFLOW);
    TEST_ASSERT(W.link.rx_state == ISOTP_IDLE);
    TEST_ASSERT(W.nframes == 1u);
    TEST_ASSERT(W.frame[0].data[0] == 0x32u);

    /* 12-bit maximum: 0x1FFF = 4095 */
    iso_fresh();
    f[0] = 0x1Fu; f[1] = 0xFFu;
    feed(f, 8u);
    TEST_ASSERT(W.link.rx_error == ISOTP_ERR_OVERFLOW);
    TEST_ASSERT(W.frame[0].data[0] == 0x32u);

    /* dlc 2 cannot hold a length field plus payload byte -> rejected */
    iso_fresh();
    f[0] = 0x10u; f[1] = 0x10u;
    feed(f, 2u);
    TEST_ASSERT(W.link.rx_state == ISOTP_IDLE);
    TEST_ASSERT(W.nframes == 0u);

    /* dlc 3 stores exactly one payload byte */
    iso_fresh();
    f[0] = 0x10u; f[1] = 0x10u; f[2] = 0x77u;
    feed(f, 3u);
    TEST_ASSERT(W.link.rx_state == ISOTP_WAIT_CF);
    TEST_ASSERT(W.link.rx_got == 1u);
    TEST_ASSERT(W.link.rx_buf[0] == 0x77u);

    /* dlc 0 and dlc 9 are not legal classic CAN frames */
    iso_fresh();
    f[0] = 0x10u; f[1] = 0x10u;
    feed(f, 0u);
    TEST_ASSERT(W.link.rx_state == ISOTP_IDLE);
}

/* ------------------------------------------------------------------ */
/* C. consecutive-frame receive boundaries                             */
/* ------------------------------------------------------------------ */
static void rx_consecutive_edges(void)
{
    uint8_t f[8];
    unsigned i;

    puts("[C] isotp_on_frame consecutive frame");

    /* sequence error: expects SN=1, receives SN=2 */
    iso_fresh();
    f[0] = 0x10u; f[1] = 0x10u; memset(&f[2], 0, 6u);
    feed(f, 8u);
    f[0] = 0x22u; memset(&f[1], 0, 7u);
    feed(f, 8u);
    TEST_ASSERT(W.link.rx_error == ISOTP_ERR_SEQUENCE);
    TEST_ASSERT(W.link.rx_state == ISOTP_IDLE);
    TEST_ASSERT(W.link.rx_done == 0u);

    /* dlc 1 carries no payload byte and must be ignored */
    iso_fresh();
    f[0] = 0x10u; f[1] = 0x10u; memset(&f[2], 0, 6u);
    feed(f, 8u);
    f[0] = 0x21u;
    feed(f, 1u);
    TEST_ASSERT(W.link.rx_state == ISOTP_WAIT_CF);
    TEST_ASSERT(W.link.rx_got == 6u);

    /* more bytes than the declared length: truncated, message completes */
    iso_fresh();
    f[0] = 0x10u; f[1] = 0x0Au; memset(&f[2], 0, 6u);
    feed(f, 8u);
    f[0] = 0x21u; memset(&f[1], 0x22, 7u);
    feed(f, 8u);
    TEST_ASSERT(W.link.rx_done == 1u);
    TEST_ASSERT(W.link.rx_got == 10u);
    TEST_ASSERT(W.link.rx_len == 10u);
    TEST_ASSERT(W.link.rx_buf[9] == 0x22u);

    /* SN wraps 0x0F -> 0x00: 118 bytes = 6 + 16*7 */
    iso_fresh();
    f[0] = 0x10u; f[1] = 118u; memset(&f[2], 0, 6u);
    feed(f, 8u);
    for (i = 0u; i < 16u; i++) {
        f[0] = (uint8_t)(0x20u | ((i + 1u) & 0x0Fu));
        memset(&f[1], (int)i, 7u);
        feed(f, 8u);
    }
    TEST_ASSERT(W.link.rx_done == 1u);
    TEST_ASSERT(W.link.rx_len == 118u);
    TEST_ASSERT(W.link.rx_got == 118u);
    TEST_ASSERT(W.link.rx_buf[117] == 15u);   /* last frame carried byte 0x0F */

    /* a second first frame while a transfer is running restarts it */
    iso_fresh();
    f[0] = 0x10u; f[1] = 0x40u; memset(&f[2], 0x01, 6u);
    feed(f, 8u);
    TEST_ASSERT(W.link.rx_got == 6u);
    f[0] = 0x10u; f[1] = 0x20u; memset(&f[2], 0x02, 6u);
    feed(f, 8u);
    TEST_ASSERT(W.link.rx_len == 0x20u);
    TEST_ASSERT(W.link.rx_got == 6u);
}

/* ------------------------------------------------------------------ */
/* D. single-frame receive boundaries                                  */
/* ------------------------------------------------------------------ */
static void rx_single_frame_edges(void)
{
    uint8_t f[8];
    uint8_t out[8];
    uint16_t n = 0u;

    puts("[D] isotp_on_frame single frame");

    /* length 0 is not a valid single frame */
    iso_fresh();
    f[0] = 0x00u;
    feed(f, 8u);
    TEST_ASSERT(W.link.rx_done == 0u);

    /* claimed length 8 with dlc 8 leaves only 7 payload bytes */
    iso_fresh();
    f[0] = 0x08u; memset(&f[1], 0x33, 7u);
    feed(f, 8u);
    TEST_ASSERT(W.link.rx_done == 0u);

    /* 7 bytes in an 8-byte frame is legal */
    iso_fresh();
    f[0] = 0x07u; memset(&f[1], 0x44, 7u);
    feed(f, 8u);
    TEST_ASSERT(W.link.rx_done == 1u);
    TEST_ASSERT(W.link.rx_len == 7u);

    /* read with a buffer that is one byte too small: data is dropped */
    iso_fresh();
    f[0] = 0x05u; memset(&f[1], 0x55, 5u);
    feed(f, 6u);
    TEST_ASSERT(W.link.rx_done == 1u);
    TEST_ASSERT(isotp_read(&W.link, out, 4u, &n) == ISOTP_ERR_OVERFLOW);
    TEST_ASSERT(W.link.rx_done == 0u);
    TEST_ASSERT(W.link.rx_len == 0u);
    TEST_ASSERT(isotp_read(&W.link, out, 4u, &n) == 0);
    NOTE(W.link.rx_error != 0, "isotp_read overflow does not record rx_error; only the return value carries it");

    /* unread message must not be overwritten by the next single frame */
    iso_fresh();
    f[0] = 0x01u; f[1] = 0xABu;
    feed(f, 2u);
    TEST_ASSERT(W.link.rx_done == 1u);
    f[1] = 0xCDu;
    feed(f, 2u);
    TEST_ASSERT(isotp_read(&W.link, out, sizeof out, &n) == 1);
    TEST_ASSERT(n == 1u && out[0] == 0xABu);
}

/* ------------------------------------------------------------------ */
/* E. block-size handshake on the transmit side                        */
/* ------------------------------------------------------------------ */
static void tx_block_edges(void)
{
    uint8_t buf[50];
    uint8_t fc[3];
    unsigned i;

    memset(buf, 0x66u, sizeof buf);

    puts("[E] isotp transmit block size / wait frames");

    /* 50 bytes: FF(6) + 7 CF.  bs=3 must stop after the third CF. */
    iso_fresh();
    TEST_ASSERT(isotp_send(&W.link, buf, 50u) == ISOTP_OK);
    fc[0] = 0x30u; fc[1] = 3u; fc[2] = 0u;
    feed(fc, 3u);
    TEST_ASSERT(W.link.tx_state == ISOTP_SENDING_CF);

    for (i = 0u; i < 3u; i++) {
        TEST_ASSERT(isotp_poll(&W.link) == 0);
    }
    TEST_ASSERT(W.link.tx_state == ISOTP_WAIT_FC);
    TEST_ASSERT(W.link.tx_sent == 27u);       /* 6 + 3*7 */

    /* the peer grants the rest */
    fc[0] = 0x30u; fc[1] = 0u; fc[2] = 0u;
    feed(fc, 3u);
    TEST_ASSERT(W.link.tx_state == ISOTP_SENDING_CF);
    for (i = 0u; i < 10u && W.link.tx_sent < 50u; i++) {
        (void)isotp_poll(&W.link);
    }
    TEST_ASSERT(W.link.tx_sent == 50u);
    TEST_ASSERT(W.link.tx_state == ISOTP_IDLE);

    /* too many WAIT frames: 1 + ISOTP_MAX_WAIT_FRAMES accepted, next aborts */
    iso_fresh();
    TEST_ASSERT(isotp_send(&W.link, buf, 20u) == ISOTP_OK);
    fc[0] = 0x31u; fc[1] = 0u; fc[2] = 0u;
    for (i = 0u; i < ISOTP_MAX_WAIT_FRAMES; i++) {
        feed(fc, 3u);
    }
    TEST_ASSERT(W.link.tx_state == ISOTP_WAIT_FC);
    feed(fc, 3u);
    TEST_ASSERT(W.link.tx_state == ISOTP_IDLE);
    TEST_ASSERT(W.link.tx_error == ISOTP_ERR_FLOW);

    /* reserved STmin: 0x80..0xF0 and 0xFA..0xFF.  ISO 15765-2 requires the
       sender to fall back to the longest legal value (0x7F = 127 ms) and keep
       transmitting.  This used to abort the whole transfer, which made one
       sloppy tester fatal. */
    iso_fresh();
    TEST_ASSERT(isotp_send(&W.link, buf, 20u) == ISOTP_OK);
    fc[0] = 0x30u; fc[1] = 0u; fc[2] = 0x80u;
    feed(fc, 3u);
    TEST_ASSERT(W.link.tx_state == ISOTP_SENDING_CF);
    TEST_ASSERT(W.link.tx_error == 0);
    TEST_ASSERT(W.link.fc_stmin == 0x7Fu);
    TEST_ASSERT(W.nframes == 1u);              /* only the first frame so far */
    W.now = 126u;
    TEST_ASSERT(isotp_poll(&W.link) == 0);
    TEST_ASSERT(W.nframes == 1u);              /* 127 ms not reached */
    W.now = 127u;
    TEST_ASSERT(isotp_poll(&W.link) == 0);
    TEST_ASSERT(W.nframes == 2u);              /* first CF finally allowed */

    /* the second reserved range 0xFA..0xFF behaves the same way */
    iso_fresh();
    TEST_ASSERT(isotp_send(&W.link, buf, 20u) == ISOTP_OK);
    fc[0] = 0x30u; fc[1] = 0u; fc[2] = 0xFAu;
    feed(fc, 3u);
    TEST_ASSERT(W.link.tx_state == ISOTP_SENDING_CF);
    TEST_ASSERT(W.link.fc_stmin == 0x7Fu);

    /* no time base at all: WAIT_FC can never expire */
    memset(&W, 0, sizeof W);
    W.port.can_send = w_send;      /* now_ms stays NULL */
    isotp_init(&W.link, &W.port, 0x7E0u, 0x7E8u);
    TEST_ASSERT(isotp_send(&W.link, buf, 20u) == ISOTP_OK);
    W.now = 0xFFFFFFFFu;
    TEST_ASSERT(isotp_poll(&W.link) == 0);
    TEST_ASSERT(W.link.tx_state == ISOTP_WAIT_FC);
    printf("  INFO %s:%d  without port.now_ms a multi-frame transfer can wait for flow control forever\n", __FILE__, __LINE__);
}

/* ------------------------------------------------------------------ */
/* F. UDS 0x22 combined DIDs and exact response-buffer sizes           */
/* ------------------------------------------------------------------ */
static bench_t B;

static void did_combination_edges(void)
{
    uint8_t req[8];
    uint8_t resp[128];
    uint16_t n;

    puts("[F] uds 0x22 combined DIDs / exact buffer sizes");

    bench_init(&B);

    /* three DIDs in one request: F190(17) + F195(5) + F186(1) */
    req[0] = 0x22u; req[1] = 0xF1u; req[2] = 0x90u;
    req[3] = 0xF1u; req[4] = 0x95u; req[5] = 0xF1u; req[6] = 0x86u;
    n = uds_handle(&B.server, req, 7u, resp, sizeof resp);
    TEST_ASSERT(n == 1u + 2u + 17u + 2u + 5u + 2u + 1u);
    TEST_ASSERT(resp[0] == 0x62u);
    TEST_ASSERT(memcmp(&resp[3], "LOTA0000000000001", 17u) == 0);
    TEST_ASSERT(memcmp(&resp[22], "0.2.0", 5u) == 0);
    TEST_ASSERT(resp[29] == UDS_SESSION_DEFAULT);

    /* exact buffer: 1 + 2 + 17 = 20 */
    req[0] = 0x22u; req[1] = 0xF1u; req[2] = 0x90u;
    n = uds_handle(&B.server, req, 3u, resp, 20u);
    TEST_ASSERT(n == 20u);

    /* one byte short */
    n = uds_handle(&B.server, req, 3u, resp, 19u);
    TEST_ASSERT(n == 3u);
    TEST_ASSERT(resp[0] == 0x7Fu && resp[1] == 0x22u);
    TEST_ASSERT(resp[2] == 0x14u);  /* responseTooLong, not generalProgrammingFailure */

    /* odd request length: 0x22 + 1.5 DIDs */
    req[0] = 0x22u; req[1] = 0xF1u;
    n = uds_handle(&B.server, req, 2u, resp, sizeof resp);
    TEST_ASSERT(n == 3u && resp[2] == 0x13u);

    /* unknown DID anywhere in the list fails the whole request */
    req[0] = 0x22u; req[1] = 0xF1u; req[2] = 0x90u;
    req[3] = 0x12u; req[4] = 0x34u;
    n = uds_handle(&B.server, req, 5u, resp, sizeof resp);
    TEST_ASSERT(n == 3u && resp[2] == 0x31u);

    /* 0x0200 status: 1 + 2 + 44 = 47, exact boundary 46 must fail cleanly */
    req[0] = 0x22u; req[1] = 0x02u; req[2] = 0x00u;
    n = uds_handle(&B.server, req, 3u, resp, 47u);
    TEST_ASSERT(n == 47u);
    TEST_ASSERT(resp[0] == 0x62u && resp[3] == 1u);
    n = uds_handle(&B.server, req, 3u, resp, 46u);
    TEST_ASSERT(n == 3u);
}

/* ------------------------------------------------------------------ */
/* G. UDS 0x36 block counter and retransmission                        */
/* ------------------------------------------------------------------ */
static void transfer_counter_edges(void)
{
    uint8_t req[64];
    uint8_t resp[16];
    uint16_t n;

    puts("[G] uds 0x36 counter / retransmission");

    bench_init(&B);
    B.server.session   = UDS_SESSION_PROGRAMMING;
    B.server.unlocked  = 1u;
    B.server.dl_active = 1u;
    B.server.dl_offset = DOWNLOAD_OFFSET;
    B.server.dl_size   = 100000u;
    B.server.dl_written = 0u;
    B.server.dl_has_previous = 0u;
    B.server.dl_next_counter = 0xFEu;

    /* first block and its retransmission (same counter, same bytes) */
    memset(req, 0xA5, sizeof req);
    req[0] = 0x36u; req[1] = 0xFEu;
    n = uds_handle(&B.server, req, 10u, resp, sizeof resp);
    TEST_ASSERT(n == 2u && resp[0] == 0x76u && resp[1] == 0xFEu);
    TEST_ASSERT(B.server.dl_written == 8u);
    TEST_ASSERT(B.server.dl_next_counter == 0xFFu);

    n = uds_handle(&B.server, req, 10u, resp, sizeof resp);
    TEST_ASSERT(n == 2u);
    TEST_ASSERT(B.server.dl_written == 8u);      /* must not advance */

    /* same counter, different payload -> 0x73 */
    req[2] = 0x5Au;
    n = uds_handle(&B.server, req, 10u, resp, sizeof resp);
    TEST_ASSERT(n == 3u && resp[2] == 0x73u);
    TEST_ASSERT(B.server.dl_written == 8u);
    req[2] = 0xA5u;

    /* counter wrap 0xFF -> 0x00 */
    req[1] = 0xFFu;
    n = uds_handle(&B.server, req, 10u, resp, sizeof resp);
    TEST_ASSERT(n == 2u && resp[1] == 0xFFu);
    TEST_ASSERT(B.server.dl_next_counter == 0x00u);
    req[1] = 0x00u;
    n = uds_handle(&B.server, req, 10u, resp, sizeof resp);
    TEST_ASSERT(n == 2u && resp[1] == 0x00u);
    TEST_ASSERT(B.server.dl_next_counter == 0x01u);

    /* out-of-order counter -> 0x73 */
    req[1] = 0x05u;
    n = uds_handle(&B.server, req, 10u, resp, sizeof resp);
    TEST_ASSERT(n == 3u && resp[2] == 0x73u);

    /* writing past the declared size -> 0x31 */
    B.server.dl_written = B.server.dl_size - 4u;
    B.server.dl_has_previous = 0u;
    B.server.dl_next_counter = 0x01u;
    req[1] = 0x01u;
    n = uds_handle(&B.server, req, 10u, resp, sizeof resp);
    TEST_ASSERT(n == 3u && resp[2] == 0x31u);
    TEST_ASSERT(B.server.dl_written == B.server.dl_size - 4u);

    /* transfer without requestDownload -> 0x24 */
    B.server.dl_active = 0u;
    n = uds_handle(&B.server, req, 10u, resp, sizeof resp);
    TEST_ASSERT(n == 3u && resp[2] == 0x24u);

    /* single byte payload is the minimum legal block */
    B.server.dl_active = 1u;
    B.server.dl_written = 0u;
    B.server.dl_has_previous = 0u;
    B.server.dl_next_counter = 0x01u;
    req[1] = 0x01u;
    n = uds_handle(&B.server, req, 3u, resp, sizeof resp);
    TEST_ASSERT(n == 2u);
    TEST_ASSERT(B.server.dl_written == 1u);
}

/* ------------------------------------------------------------------ */
/* H. UDS security edges                                               */
/* ------------------------------------------------------------------ */
static void security_edges(void)
{
    uint8_t req[8];
    uint8_t resp[16];
    uint16_t n;

    puts("[H] uds 0x27 security edges");

    bench_init(&B);

    /* sendKey without a prior seed */
    req[0] = 0x27u; req[1] = 0x02u; memset(&req[2], 0, 4u);
    n = uds_handle(&B.server, req, 6u, resp, sizeof resp);
    TEST_ASSERT(n == 3u && resp[2] == 0x24u);

    /* sendKey with a 5-byte body */
    req[0] = 0x27u; req[1] = 0x02u; memset(&req[2], 0, 3u);
    n = uds_handle(&B.server, req, 5u, resp, sizeof resp);
    TEST_ASSERT(n == 3u && resp[2] == 0x13u);

    /* seed, then a correct key */
    req[0] = 0x27u; req[1] = 0x01u;
    n = uds_handle(&B.server, req, 2u, resp, sizeof resp);
    TEST_ASSERT(n == 6u && resp[0] == 0x67u && resp[1] == 0x01u);

    /* 0x27 must not use the suppress bit; bit 7 makes the sub-function unknown */
    {
        uint8_t s[2] = {0x27u, 0x81u};
        n = uds_handle(&B.server, s, 2u, resp, sizeof resp);
        NOTE(n == 3u && resp[2] == 0x12u,
             "0x27 sub-function is not masked with 0x7F; 0x81 is rejected as unknown (ISO 14229 does not allow suppress for 0x27, so this is defensible)");
    }

    /* unlocked seed request returns an all-zero seed */
    B.server.unlocked = 1u;
    req[0] = 0x27u; req[1] = 0x01u;
    n = uds_handle(&B.server, req, 2u, resp, sizeof resp);
    TEST_ASSERT(n == 6u);
    TEST_ASSERT(t_get_be32(&resp[2]) == 0u);
}

/* ------------------------------------------------------------------ */
/* I. session / response-size interaction                              */
/* ------------------------------------------------------------------ */
static void session_response_edges(void)
{
    uint8_t req[4];
    uint8_t resp[16];
    uint16_t n;

    puts("[I] uds response buffer preconditions");

    bench_init(&B);

    req[0] = 0x10u; req[1] = 0x03u;
    n = uds_handle(&B.server, req, 2u, resp, 5u);
    TEST_ASSERT(n == 0u);                 /* cannot fit the 6-byte response */
    TEST_ASSERT(B.server.session == UDS_SESSION_DEFAULT);  /* must not change state */

    n = uds_handle(&B.server, req, 2u, resp, 6u);
    TEST_ASSERT(n == 6u && B.server.session == UDS_SESSION_EXTENDED);

    /* 0x11 with a 2-byte buffer: no room for the NRC, no response */
    req[0] = 0x11u; req[1] = 0x09u;
    n = uds_handle(&B.server, req, 2u, resp, 2u);
    TEST_ASSERT(n == 0u);

    /* 0x3E in a 3-byte buffer */
    req[0] = 0x3Eu; req[1] = 0x00u;
    n = uds_handle(&B.server, req, 2u, resp, 3u);
    TEST_ASSERT(n == 2u && resp[0] == 0x7Eu);

    /* unsupported SID still produces a 3-byte NRC */
    req[0] = 0x99u;
    n = uds_handle(&B.server, req, 1u, resp, 3u);
    TEST_ASSERT(n == 3u && resp[0] == 0x7Fu && resp[1] == 0x99u && resp[2] == 0x11u);
}

/* ------------------------------------------------------------------ */
void run_input_edge_tests(void)
{
    puts("[input edges]");
    send_length_edges();
    rx_first_frame_edges();
    rx_consecutive_edges();
    rx_single_frame_edges();
    tx_block_edges();
    did_combination_edges();
    transfer_counter_edges();
    security_edges();
    session_response_edges();
}
