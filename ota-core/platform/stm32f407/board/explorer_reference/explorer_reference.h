#ifndef OTA_EXPLORER_REFERENCE_H
#define OTA_EXPLORER_REFERENCE_H
#include "board_hal_user.h"
#include "ota_stm32.h"

/* Common reference wiring verified in ATK V2 schematic and V3 manual.
 * This does not identify the user's physical PCB or fitted NOR part. */
#ifndef OTA_EXPLORER_REFERENCE_PINS
#define OTA_EXPLORER_REFERENCE_PINS 0
#endif
#ifndef OTA_EXPLORER_CAN_LOOPBACK
#define OTA_EXPLORER_CAN_LOOPBACK 1
#endif
/* Wire the W25Q128-shaped protocol driver into the three ota_user_nor_* hooks.
 * Capability only: the update engine still needs storage_verified, which this
 * bring-up build keeps at 0. Set to 0 for a build with no write code at all. */
#ifndef OTA_EXPLORER_NOR_WRITE
#define OTA_EXPLORER_NOR_WRITE 1
#endif
/* Destructive erase/program/readback of ONE sector at boot. Off by default;
 * enabling it requires OTA_EXPLORER_NOR_SELFTEST_ADDR plus a restore plan. */
#ifndef OTA_EXPLORER_NOR_SELFTEST
#define OTA_EXPLORER_NOR_SELFTEST 0
#endif
#if OTA_EXPLORER_NOR_SELFTEST
#if !defined(OTA_EXPLORER_NOR_SELFTEST_ADDR)
#error "OTA_EXPLORER_NOR_SELFTEST requires OTA_EXPLORER_NOR_SELFTEST_ADDR"
#elif (OTA_EXPLORER_NOR_SELFTEST_ADDR % 4096u) != 0u
#error "Select a 4 KiB aligned sector for the write self-test"
#elif OTA_EXPLORER_NOR_SELFTEST_ADDR < EXT_FLASH_TOTAL_SIZE
#error "The self-test sector must stay outside the new/old image slots"
#endif
#if OTA_EXPLORER_NOR_WRITE != 1
#error "The write self-test needs the NOR write hooks compiled in"
#endif
#endif
#if OTA_ENABLE_FLASHING || OTA_ENABLE_BOOT_HANDOFF || OTA_ENABLE_WATCHDOG
#error "Explorer reference bring-up targets are read-only; all update gates must stay off"
#endif
#if OTA_EXPLORER_CAN_LOOPBACK != 0 && OTA_EXPLORER_CAN_LOOPBACK != 1
#error "Select silent internal loopback (1) or normal physical CAN (0)"
#endif

#define OTA_EXPLORER_HSE_HZ 8000000u
#define OTA_EXPLORER_HCLK_HZ 168000000u
#define OTA_EXPLORER_PCLK1_HZ 42000000u
#define OTA_EXPLORER_PCLK2_HZ 84000000u
#define OTA_EXPLORER_CAN_PRESCALER 6u
#define OTA_EXPLORER_CAN_BS1_TQ 11u
#define OTA_EXPLORER_CAN_BS2_TQ 2u
#define OTA_EXPLORER_CAN_BITRATE 500000u

typedef struct {
    uint32_t device_id,revision_id,flash_bytes;
    uint32_t hclk_hz,pclk1_hz,pclk2_hz;
    uint32_t jedec_id,eeprom_crc32;
    int jedec_status,eeprom_status;
    /* Driver probe result, capacity and a 256-byte dry read at offset 0.
     * Proxy for "the bus and the chip answer": not a write or wear test. */
    uint32_t nor_bytes,nor_read_crc32;
    int nor_status,nor_read_status;
    uint8_t eeprom[256]; /* Only a readback; no automatic metadata initialization. */
} ota_explorer_probe_t;
extern ota_explorer_probe_t ota_explorer_probe;
/* Implemented by the selected firmware main; called from FIFO0 ISR. */
void ota_bootloader_can_rx(const can_frame_t *frame);
/* Implemented by the selected firmware main; called from USART1 ISR. */
void ota_user_uart_rx(uint8_t byte);
#endif
