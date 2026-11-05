/* Temporary: play the attacker who has a debug probe and no private key.
 *
 * The question the secure-boot ticket exists to answer is the one CRC32
 * cannot: if someone rewrites the installed app and then repairs every
 * checksum they are able to compute -- the payload CRC in the ticket header,
 * the header's own CRC32, and active_crc in the EEPROM record -- what is left
 * to stop the board from running it?
 *
 * This image does exactly that, then parks. Reflash the real loader and reset:
 * the loader must refuse with "ticket signature does not verify". Anything
 * less than that refusal means the ticket is decoration.
 *
 * Links at 0x08000000 and replaces the loader, because that is the only way
 * to run code that is not itself subject to the check. Never shipped.
 */
#include "ota_stm32.h"
#include "ota/flash_layout.h"
#include "ota/crc32.h"
#include "ota/image.h"
#include "explorer_reference.h"
#include <string.h>

/* active_size is 44824 on the board this was written for; 64 KiB leaves room
 * to tamper with any image this project has ever installed. */
static uint8_t app_copy[64u * 1024u];
static uint8_t ticket_copy[APP_TICKET_SIZE];
static uint8_t chunk[1024];

static void log_hex(const char *label, uint32_t value)
{
    static const char hex[] = "0123456789ABCDEF";
    char line[64];
    size_t n = strlen(label);
    uint32_t i;
    if (n > sizeof line - 12u) return;
    memcpy(line, label, n); line[n++] = '='; line[n++] = '0'; line[n++] = 'x';
    for (i = 0u; i < 8u; i++) line[n++] = hex[(value >> (28u - 4u * i)) & 15u];
    line[n] = 0;
    ota_bsp_log(line);
}

int ota_firmware_watchdog_service(void) { return 0; }
int ota_firmware_service(void) { return 0; }
void ota_user_uart_rx(uint8_t byte) { (void)byte; }
void ota_bootloader_can_rx(const can_frame_t *frame) { (void)frame; }


int main(void)
{
    ota_board_config_t config;
    ota_stm32_ports_t storage;
    metadata_t m;
    image_header_t h;
    uint32_t size, off, crc;

    ota_arch_irq_enable();
    if (ota_bsp_init(&config) || !config.board_verified) {
        for (;;) ota_bsp_idle();
    }
    ota_stm32_ports_init(&storage, &config);
    if (!storage.storage_ready || metadata_load(&storage.store, &m) != 1) {
        ota_bsp_log("attacker: no usable metadata, nothing to forge");
        for (;;) ota_bsp_idle();
    }
    size = m.active_size;
    log_hex("active_version", m.active_version);
    log_hex("active_size", size);
    if (size < 8u || size > sizeof app_copy) {
        ota_bsp_log("attacker: active image does not fit the workspace");
        for (;;) ota_bsp_idle();
    }

    /* Keep both halves in RAM: the erase below removes the app and its ticket
     * in one call, and the whole point is to put both back with matching
     * checksums. */
    for (off = 0u; off < size; off += sizeof chunk) {
        uint32_t n = size - off;
        if (n > sizeof chunk) n = sizeof chunk;
        if (storage.internal.read(storage.internal.user, APP_OFFSET + off,
                                  app_copy + off, n)) {
            ota_bsp_log("attacker: app read failed");
            for (;;) ota_bsp_idle();
        }
    }
    if (storage.internal.read(storage.internal.user, APP_TICKET_OFFSET,
                              ticket_copy, sizeof ticket_copy)) {
        ota_bsp_log("attacker: ticket read failed");
        for (;;) ota_bsp_idle();
    }

    /* Tamper. One byte in the middle of the payload is enough: the point is
     * not how much changes, it is that the board cannot tell. */
    app_copy[size / 2u] ^= 0x5Au;
    crc = crc32_calc(app_copy, size);
    log_hex("forged_payload_crc", crc);

    image_header_decode(&h, ticket_copy);
    h.payload_crc = crc;
    h.image_size = size;
    image_header_finalize(&h);
    image_header_encode(&h, ticket_copy);   /* header CRC32 now matches too */
    log_hex("forged_header_crc", h.header_crc);

    if (storage.internal.erase(storage.internal.user, APP_OFFSET, APP_SIZE)) {
        ota_bsp_log("attacker: erase failed");
        for (;;) ota_bsp_idle();
    }
    {
        /* storage_module_internal_write() is the port write; the board object
         * is reached through it to keep this file free of HAL includes. */
        uint32_t done = 0u;
        while (done < size) {
            uint32_t piece = size - done;
            if (piece > sizeof chunk) piece = sizeof chunk;
            memset(chunk, 0xFF, sizeof chunk);
            memcpy(chunk, app_copy + done, piece);
            if (storage.internal.write(storage.internal.user, APP_OFFSET + done,
                                       chunk, (piece + 3u) & ~3u)) {
                ota_bsp_log("attacker: payload write failed");
                for (;;) ota_bsp_idle();
            }
            done += piece;
        }
    }
    if (storage.internal.write(storage.internal.user, APP_TICKET_OFFSET,
                               ticket_copy, sizeof ticket_copy)) {
        ota_bsp_log("attacker: ticket write failed");
        for (;;) ota_bsp_idle();
    }

    /* Last step: make the record agree with the forged bytes. After this,
     * every check that does not need a key passes. */
    m.active_crc = crc;
    if (metadata_save(&storage.store, &m)) {
        ota_bsp_log("attacker: metadata write failed");
        for (;;) ota_bsp_idle();
    }
    ota_bsp_log("attacker: forged image installed with matching CRCs");
    ota_bsp_log("attacker: reflash the loader and reset to see the refusal");
    for (;;) ota_bsp_idle();
}
