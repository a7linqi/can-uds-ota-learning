#include <stdio.h>
#include <string.h>

#include "ota/can_uart.h"
#include "test_util.h"

static int decode_bytes(const uint8_t *raw, uint32_t len, can_frame_t *out)
{
    can_uart_decoder_t decoder;
    uint32_t i;
    can_uart_decoder_init(&decoder);
    for (i = 0u; i < len; i++) {
        if (can_uart_decoder_feed(&decoder, raw[i], out) == 1) {
            return 1;
        }
    }
    return 0;
}

void run_uart_codec_tests(void)
{
    can_frame_t frame, decoded;
    uint8_t raw[CAN_UART_MAX_FRAME];
    uint32_t n;
    int i;

    printf("[uart codec]\n");

    TEST_ASSERT(can_uart_crc16((const uint8_t *)"123456789", 9u) == 0x29B1u);

    memset(&frame, 0, sizeof frame);
    frame.id = 0x7E8u;
    frame.dlc = 8u;
    for (i = 0; i < 8; i++) frame.data[i] = (uint8_t)i;
    n = can_uart_encode(&frame, raw, sizeof raw);
    TEST_ASSERT(n == 18u);
    TEST_ASSERT(raw[0] == CAN_UART_SYNC0 && raw[1] == CAN_UART_SYNC1);
    TEST_ASSERT(raw[2] == 0x01u && raw[3] == 0x08u);
    TEST_ASSERT(raw[4] == 0x00u && raw[5] == 0x00u && raw[6] == 0x07u && raw[7] == 0xE8u);
    TEST_ASSERT(raw[16] == 0x8Du && raw[17] == 0x70u);
    TEST_ASSERT(decode_bytes(raw, n, &decoded) == 1);
    TEST_ASSERT(decoded.id == frame.id && decoded.dlc == frame.dlc && !decoded.extended);
    TEST_ASSERT(memcmp(decoded.data, frame.data, 8u) == 0);

    memset(&frame, 0, sizeof frame);
    frame.id = 0x18DAF110u;
    frame.extended = true;
    frame.dlc = 3u;
    frame.data[0] = 1u; frame.data[1] = 2u; frame.data[2] = 3u;
    n = can_uart_encode(&frame, raw, sizeof raw);
    TEST_ASSERT(n == 13u);
    TEST_ASSERT(raw[2] == (CAN_UART_VERSION | CAN_UART_EXTENDED));
    TEST_ASSERT(decode_bytes(raw, n, &decoded) == 1);
    TEST_ASSERT(decoded.id == frame.id && decoded.extended && decoded.dlc == 3u);
    TEST_ASSERT(memcmp(decoded.data, frame.data, 3u) == 0);

    /* Noise and a lone sync byte must not prevent a valid frame. */
    {
        uint8_t noisy[24], encoded[CAN_UART_MAX_FRAME];
        can_frame_t small;
        memset(&small, 0, sizeof small);
        small.id = 0x7E0u; small.dlc = 3u;
        small.data[0] = 2u; small.data[1] = 0x10u; small.data[2] = 0x03u;
        n = can_uart_encode(&small, encoded, sizeof encoded);
        TEST_ASSERT(n > 0u);
        noisy[0] = 0x00u; noisy[1] = 0xFFu;
        noisy[2] = CAN_UART_SYNC0; noisy[3] = 0x11u;
        memcpy(noisy + 4u, encoded, n);
        TEST_ASSERT(decode_bytes(noisy, n + 4u, &decoded) == 1);
        TEST_ASSERT(decoded.id == 0x7E0u && decoded.dlc == 3u);
    }

    /* Corrupted CRC is rejected, and a following valid frame still decodes. */
    {
        uint8_t combined[40];
        can_frame_t small;
        memset(&small, 0, sizeof small);
        small.id = 0x7E0u; small.dlc = 3u;
        small.data[0] = 2u; small.data[1] = 0x10u; small.data[2] = 0x03u;
        n = can_uart_encode(&small, raw, sizeof raw);
        TEST_ASSERT(n > 0u);
        memcpy(combined, raw, n);
        combined[n - 1u] ^= 0x01u;
        TEST_ASSERT(decode_bytes(combined, n, &decoded) == 0);
        n = can_uart_encode(&small, raw, sizeof raw);
        memcpy(combined + 18u, raw, n);
        TEST_ASSERT(decode_bytes(combined + 18u, n, &decoded) == 1);
    }

    /* Invalid DLC and invalid standard ID are rejected by the decoder. */
    {
        uint8_t bad[16];
        memset(bad, 0, sizeof bad);
        bad[0] = CAN_UART_SYNC0; bad[1] = CAN_UART_SYNC1;
        bad[2] = 0x01u; bad[3] = 0x09u;
        TEST_ASSERT(decode_bytes(bad, 8u, &decoded) == 0);

        memset(bad, 0, sizeof bad);
        bad[0] = CAN_UART_SYNC0; bad[1] = CAN_UART_SYNC1;
        bad[2] = 0x01u; bad[3] = 0x01u;
        bad[4] = 0x00u; bad[5] = 0x00u; bad[6] = 0x08u; bad[7] = 0x00u;
        bad[8] = 0xAAu;
        bad[9] = 0x00u; bad[10] = 0x00u;
        TEST_ASSERT(decode_bytes(bad, 11u, &decoded) == 0);
    }

    /* Encoder rejects out-of-range IDs and short output buffers. */
    memset(&frame, 0, sizeof frame);
    frame.id = 0x800u; frame.dlc = 1u;
    TEST_ASSERT(can_uart_encode(&frame, raw, sizeof raw) == 0u);
    frame.extended = true;
    TEST_ASSERT(can_uart_encode(&frame, raw, 10u) == 0u);
    frame.id = 0x7E0u; frame.dlc = 8u;
    TEST_ASSERT(can_uart_encode(&frame, raw, 10u) == 0u);
    frame.dlc = 0u;
    TEST_ASSERT(can_uart_encode(&frame, raw, sizeof raw) == 0u);
}
