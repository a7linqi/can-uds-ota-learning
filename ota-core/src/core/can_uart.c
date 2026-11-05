/* CAN-over-UART frame decoder with sliding-window resynchronisation.
 *
 * Wire format, ota/can_uart.h and can_uart_encode() are unchanged.
 *
 * A truncated frame (sender reset, lost byte, noise) used to leave the old
 * decoder in DECODER_COLLECT with an `expected` length taken from a byte
 * that was never really a header. It kept swallowing bytes -- including the
 * A5 5A of the next real frame -- until that length was reached, then failed
 * the CRC and reset. One bad byte therefore cost at least one good frame.
 *
 * parse_front() now classifies the bytes at the front on every new byte.
 * When they can never start a valid frame, one byte is dropped and the window
 * walks to the next sync marker. A complete valid frame at the front is
 * always emitted before resynchronisation is considered, so a payload that
 * legitimately contains A5 5A is not cut in half.
 *
 * Known limit: if a fake sync header claims 18 bytes, only 17 bytes follow
 * and the line then goes idle, the decoder cannot disprove it until one more
 * byte arrives. There is no flush()/timeout hook. A continuous UART stream
 * supplies that byte; a link that stops mid-frame needs one.
 */
#include "ota/can_uart.h"
#include <string.h>

/* Kept for compatibility and for debugger inspection only; the decoder no
 * longer needs them to make progress. */
enum {
    DECODER_WAIT_SYNC0 = 0,
    DECODER_WAIT_SYNC1,
    DECODER_COLLECT
};

static void reset(can_uart_decoder_t *d)
{
    d->len = 0u;
    d->expected = 0u;
    d->state = DECODER_WAIT_SYNC0;
}

static void update_state(can_uart_decoder_t *d)
{
    if (d->len == 0u) {
        d->state = DECODER_WAIT_SYNC0;
    } else if (d->len == 1u && d->buf[0] == CAN_UART_SYNC0) {
        d->state = DECODER_WAIT_SYNC1;
    } else {
        d->state = DECODER_COLLECT;
    }
}

void can_uart_decoder_init(can_uart_decoder_t *d)
{
    if (d != 0) {
        memset(d, 0, sizeof *d);
        d->state = DECODER_WAIT_SYNC0;
    }
}

uint16_t can_uart_crc16(const uint8_t *data, uint32_t len)
{
    uint16_t crc = 0xFFFFu;
    uint32_t i;
    uint8_t bit;

    for (i = 0u; i < len; i++) {
        crc ^= (uint16_t)((uint16_t)data[i] << 8u);
        for (bit = 0u; bit < 8u; bit++) {
            if (crc & 0x8000u) {
                crc = (uint16_t)((uint16_t)(crc << 1u) ^ 0x1021u);
            } else {
                crc = (uint16_t)(crc << 1u);
            }
        }
    }
    return crc;
}

uint32_t can_uart_encode(const can_frame_t *frame, uint8_t *out, uint32_t max)
{
    uint32_t total, i;
    uint16_t crc;

    if (frame == 0 || out == 0 || frame->dlc == 0u || frame->dlc > 8u) {
        return 0u;
    }
    if (frame->extended) {
        if (frame->id > 0x1FFFFFFFu) {
            return 0u;
        }
    } else if (frame->id > 0x7FFu) {
        return 0u;
    }
    total = 10u + frame->dlc;
    if (total > max) {
        return 0u;
    }
    out[0] = CAN_UART_SYNC0;
    out[1] = CAN_UART_SYNC1;
    out[2] = (uint8_t)(CAN_UART_VERSION |
                       (frame->extended ? CAN_UART_EXTENDED : 0u));
    out[3] = frame->dlc;
    out[4] = (uint8_t)(frame->id >> 24u);
    out[5] = (uint8_t)(frame->id >> 16u);
    out[6] = (uint8_t)(frame->id >> 8u);
    out[7] = (uint8_t)frame->id;
    for (i = 0u; i < frame->dlc; i++) {
        out[8u + i] = frame->data[i];
    }
    crc = can_uart_crc16(&out[2], 6u + frame->dlc);
    out[8u + frame->dlc] = (uint8_t)(crc >> 8u);
    out[9u + frame->dlc] = (uint8_t)crc;
    return total;
}

/* Examine the bytes at the front of the window.
 *  1 = a complete, valid frame was consumed into *out
 *  0 = more bytes are needed before the front can be classified
 * -1 = the front can never start a valid frame; drop one byte and retry
 */
static int parse_front(can_uart_decoder_t *d, can_frame_t *out)
{
    uint8_t version, dlc;
    uint32_t id, total;
    uint16_t crc;
    int i;

    if (d->len < 1u) {
        return 0;
    }
    if (d->buf[0] != CAN_UART_SYNC0) {
        return -1;
    }
    if (d->len < 2u) {
        return 0;
    }
    if (d->buf[1] != CAN_UART_SYNC1) {
        return -1;
    }
    if (d->len < 4u) {
        return 0;
    }
    version = d->buf[2];
    dlc = d->buf[3];
    if (dlc == 0u || dlc > 8u) {
        return -1;
    }
    total = 10u + dlc;
    if (d->len < total) {
        d->expected = (uint8_t)total;   /* informational: what the front still needs */
        return 0;
    }
    if ((version & 0x7Fu) != CAN_UART_VERSION) {
        return -1;
    }
    id = ((uint32_t)d->buf[4] << 24u) | ((uint32_t)d->buf[5] << 16u) |
         ((uint32_t)d->buf[6] << 8u) | (uint32_t)d->buf[7];
    if (((version & CAN_UART_EXTENDED) != 0u && id > 0x1FFFFFFFu) ||
        ((version & CAN_UART_EXTENDED) == 0u && id > 0x7FFu)) {
        return -1;
    }
    crc = (uint16_t)((uint16_t)d->buf[8u + dlc] << 8u) |
          (uint16_t)d->buf[9u + dlc];
    if (crc != can_uart_crc16(&d->buf[2], 6u + dlc)) {
        return -1;
    }

    memset(out, 0, sizeof *out);
    out->id = id;
    out->dlc = dlc;
    out->extended = (version & CAN_UART_EXTENDED) != 0u;
    for (i = 0; i < (int)dlc; i++) {
        out->data[i] = d->buf[8u + (uint8_t)i];
    }
    d->len = (uint8_t)(d->len - total);
    memmove(d->buf, &d->buf[total], d->len);
    d->expected = 0u;
    return 1;
}

static void drop_front(can_uart_decoder_t *d)
{
    d->len--;
    memmove(d->buf, &d->buf[1], d->len);
}

int can_uart_decoder_feed(can_uart_decoder_t *d, uint8_t byte, can_frame_t *out)
{
    int result;

    if (d == 0 || out == 0) {
        return 0;
    }
    /* The window only ever needs to hold one maximal frame: 2 sync + 8 header
     * + 8 data. If it is full and still unresolved, the oldest byte is dead. */
    if (d->len >= CAN_UART_MAX_FRAME) {
        drop_front(d);
    }
    d->buf[d->len++] = byte;

    for (;;) {
        result = parse_front(d, out);
        if (result > 0) {
            update_state(d);
            return 1;
        }
        if (result == 0) {
            update_state(d);
            return 0;
        }
        drop_front(d);
        if (d->len == 0u) {
            reset(d);
            return 0;
        }
    }
}
