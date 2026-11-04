#include "ota/can_uart.h"
#include <string.h>

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

int can_uart_decoder_feed(can_uart_decoder_t *d, uint8_t byte, can_frame_t *out)
{
    uint8_t version, dlc;
    uint32_t id;
    uint16_t crc;
    int i;

    if (d == 0 || out == 0) {
        return 0;
    }
    if (d->state == DECODER_WAIT_SYNC0) {
        if (byte == CAN_UART_SYNC0) {
            d->state = DECODER_WAIT_SYNC1;
        }
        return 0;
    }
    if (d->state == DECODER_WAIT_SYNC1) {
        if (byte == CAN_UART_SYNC1) {
            d->state = DECODER_COLLECT;
            d->len = 0u;
            d->expected = 0u;
        } else if (byte != CAN_UART_SYNC0) {
            d->state = DECODER_WAIT_SYNC0;
        }
        return 0;
    }

    if (d->len >= CAN_UART_MAX_FRAME) {
        reset(d);
        return 0;
    }
    d->buf[d->len++] = byte;
    if (d->len == 2u) {
        dlc = d->buf[1];
        if (dlc == 0u || dlc > 8u) {
            reset(d);
            return 0;
        }
        d->expected = (uint8_t)(8u + dlc);
    }
    if (d->expected == 0u || d->len < d->expected) {
        return 0;
    }
    if (d->len != d->expected) {
        reset(d);
        return 0;
    }

    version = d->buf[0];
    dlc = d->buf[1];
    id = ((uint32_t)d->buf[2] << 24u) | ((uint32_t)d->buf[3] << 16u) |
         ((uint32_t)d->buf[4] << 8u) | (uint32_t)d->buf[5];
    if ((version & 0x7Fu) != CAN_UART_VERSION) {
        reset(d);
        return 0;
    }
    if (((version & CAN_UART_EXTENDED) != 0u && id > 0x1FFFFFFFu) ||
        ((version & CAN_UART_EXTENDED) == 0u && id > 0x7FFu)) {
        reset(d);
        return 0;
    }
    crc = (uint16_t)((uint16_t)d->buf[6u + dlc] << 8u) |
          (uint16_t)d->buf[7u + dlc];
    if (crc != can_uart_crc16(d->buf, 6u + dlc)) {
        reset(d);
        return 0;
    }

    memset(out, 0, sizeof *out);
    out->id = id;
    out->dlc = dlc;
    out->extended = (version & CAN_UART_EXTENDED) != 0u;
    for (i = 0; i < (int)dlc; i++) {
        out->data[i] = d->buf[6u + (uint8_t)i];
    }
    reset(d);
    return 1;
}
