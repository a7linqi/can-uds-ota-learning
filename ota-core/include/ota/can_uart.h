#ifndef OTA_CAN_UART_H
#define OTA_CAN_UART_H

#include <stdint.h>
#include "ota/can_frame.h"

/* Bench-only CAN-over-UART wire format:
 *   A5 5A | ver_flags | dlc | id[4 big-endian] | data[dlc] | crc16[2 big-endian]
 * crc16 covers ver_flags..last data byte (CRC-16/CCITT-FALSE).
 * This carries classic CAN frames so ISO-TP and UDS remain transport-neutral;
 * it is not a vehicle bus protocol and must not be used in production. */
#define CAN_UART_SYNC0       0xA5u
#define CAN_UART_SYNC1       0x5Au
#define CAN_UART_VERSION     0x01u
#define CAN_UART_EXTENDED    0x80u
#define CAN_UART_MAX_FRAME   18u

typedef struct {
    uint8_t buf[CAN_UART_MAX_FRAME];
    uint8_t len;
    uint8_t expected;
    uint8_t state;
} can_uart_decoder_t;

void can_uart_decoder_init(can_uart_decoder_t *d);
/* Feed one byte. Returns 1 only when a complete, CRC-valid frame was decoded. */
int can_uart_decoder_feed(can_uart_decoder_t *d, uint8_t byte, can_frame_t *out);
/* Returns the wire length, or 0 when the frame/max length is invalid. */
uint32_t can_uart_encode(const can_frame_t *frame, uint8_t *out, uint32_t max);
uint16_t can_uart_crc16(const uint8_t *data, uint32_t len);

#endif /* OTA_CAN_UART_H */
