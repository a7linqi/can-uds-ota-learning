#ifndef OTA_IMAGE_H
#define OTA_IMAGE_H
#include <stdint.h>
#include "ota/flash_layout.h"
#include "ota/ota_port.h"
#define IMAGE_MAGIC 0x32474D49u /* IMG2, explicitly little-endian on wire */
/* flags bit 0: an Ed25519 signature (OTA_SIGNATURE_SIZE bytes) follows the
 * payload. Signed message = 32-byte header + payload. See ota/signature.h. */
#define IMAGE_FLAG_SIGNED 0x00000001u
/* When 1, every image MUST carry a valid Ed25519 signature; unsigned images
 * are rejected. This is the default and closes the "CRC only detects
 * corruption, not forgery" gap. */
#ifndef OTA_REQUIRE_SIGNATURE
#define OTA_REQUIRE_SIGNATURE 1
#endif
typedef struct {
    uint32_t magic, version, image_size, payload_crc;
    uint32_t load_address, flags, reserved, header_crc;
} image_header_t;
void image_header_finalize(image_header_t *h);
int image_header_ok(const image_header_t *h);
void image_header_encode(const image_header_t *h, uint8_t out[IMAGE_HEADER_SIZE]);
void image_header_decode(image_header_t *h, const uint8_t in[IMAGE_HEADER_SIZE]);
/* Only DOWNLOAD_OFFSET/BACKUP_OFFSET in external flash are accepted. */
int image_read_header(flash_port_t external, uint32_t offset, image_header_t *h);
int image_validate(flash_port_t external, uint32_t offset,
                   uint32_t *version, uint32_t *payload_size);
/* Internal APP_OFFSET contains payload/vectors, NOT a container header.
 *
 * Why this one call has three outcomes while the others answer yes/no: the
 * boot selector has to tell "this application is wrong" from "this application
 * could not be read". The first means the recovery target is already gone and
 * the only useful move is to install over it; the second means the flash bus
 * hiccuped and the next reset may well read the same image fine. Collapsing
 * both into -1 is what let an unreadable-but-possibly-good App wedge the
 * update state machine in READY forever. */
#define IMAGE_APP_VALID      0
#define IMAGE_APP_CORRUPT  (-1)
#define IMAGE_APP_UNREADABLE (-2)
int image_validate_app(flash_port_t internal, uint32_t size, uint32_t crc);

/* Secure-boot ticket: the 32-byte container header and the 64-byte Ed25519
 * signature of the image at APP_OFFSET, stored in the last APP_TICKET_SIZE
 * bytes of the app area (see ota/flash_layout.h).
 *
 * These two only move bytes. The boot selector is the only caller that also
 * holds the metadata needed to decide whether the bytes describe the app it
 * is about to jump into, and it is the one that runs the signature check. */
int image_ticket_read(flash_port_t internal, uint8_t raw[IMAGE_HEADER_SIZE],
                      uint8_t signature[OTA_SIGNATURE_SIZE]);
int image_ticket_write(flash_port_t internal, const uint8_t raw[IMAGE_HEADER_SIZE],
                       const uint8_t signature[OTA_SIGNATURE_SIZE]);

/* Only BACKUP_OFFSET is accepted, and the container MUST be signed.
 *
 * It used to be CRC-only: the device has no private key, so it cannot re-sign
 * at restore time. The ticket removes that constraint -- create_backup()
 * copies the signature the device already holds into the backup container,
 * so a restore replays a proof instead of producing one. An unsigned backup
 * is now rejected because it would be the one path that puts unverified code
 * into internal flash. */
int image_validate_backup(flash_port_t external, uint32_t offset,
                          uint32_t *version, uint32_t *size);
#endif
