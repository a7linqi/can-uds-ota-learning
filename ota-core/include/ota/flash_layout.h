#ifndef OTA_FLASH_LAYOUT_H
#define OTA_FLASH_LAYOUT_H

/* F407ZG default. A ZE build must override FLASH_TOTAL_SIZE to 0x80000.
 * Internal flash contains ONE executable app, with its vector table at
 * APP_BASE_ADDR. Package headers exist ONLY in external NOR flash. */
#define FLASH_BASE_ADDR       0x08000000u
#ifndef FLASH_TOTAL_SIZE
#define FLASH_TOTAL_SIZE      0x00100000u
#endif
#define BOOT_OFFSET           0u
#define BOOT_SIZE             0x00010000u
#define APP_OFFSET            BOOT_SIZE
#define APP_BASE_ADDR         (FLASH_BASE_ADDR + APP_OFFSET)
#define APP_SIZE              (FLASH_TOTAL_SIZE - APP_OFFSET)

/* Secure-boot ticket: the 32-byte container header and the 64-byte Ed25519
 * signature of whatever image currently sits at APP_OFFSET.
 *
 * It is stored at the END of the app area on purpose. The erase that removes
 * an app removes its proof in the same call, so a half-written image can never
 * be paired with the ticket of the image it replaced, and no second storage
 * format (a NOR voucher region, another EEPROM record) has to be invented and
 * kept in sync with the install transaction.
 *
 * The consequence is that an app may be at most APP_MAX_SIZE bytes:
 * 960 KiB - 96 B on the 1 MiB part, 448 KiB - 96 B on the 512 KiB part. The
 * images this project ships are ~44 KiB, so this is not a capacity decision.
 *
 * The ticket is NOT trusted for anything except as input to a signature
 * check. Its header is inside the signed message, so editing it only makes
 * the signature fail. */
#define APP_TICKET_SIZE       96u   /* header + signature */
#define APP_MAX_SIZE          (APP_SIZE - APP_TICKET_SIZE)
#define APP_TICKET_OFFSET     (APP_OFFSET + APP_MAX_SIZE)

#if (APP_TICKET_OFFSET & 3u) != 0u
#error The secure-boot ticket must be word aligned for internal flash writes
#endif

/* Two external image containers; 2 MiB is the required allocated region,
 * not a claim about the capacity/model fitted to the purchased board. */
#define EXT_IMAGE_SLOT_SIZE   0x00100000u
#define DOWNLOAD_OFFSET       0u
#define BACKUP_OFFSET         EXT_IMAGE_SLOT_SIZE
#define EXT_FLASH_TOTAL_SIZE  (2u * EXT_IMAGE_SLOT_SIZE)
#define EXT_ERASE_SIZE        4096u
#define IMAGE_HEADER_SIZE     32u
/* The ticket eats the last 96 bytes of the app area, so an image payload can
 * never be APP_SIZE long. Everything that bounds a package size derives from
 * this value and follows automatically. */
#define IMAGE_MAX_PAYLOAD     APP_MAX_SIZE
#define OTA_SIGNATURE_SIZE    64u

/* AT24C02: 2 Kbit = 256 bytes. Records occupy independent page ranges. */
#define EEPROM_TOTAL_SIZE     256u
#define EEPROM_PAGE_SIZE      8u
#define META_A_OFFSET         0u
#define META_B_OFFSET         128u
#define META_RECORD_SIZE      64u
#define META_COPY_COUNT       2u

#if FLASH_TOTAL_SIZE != 0x00100000u && FLASH_TOTAL_SIZE != 0x00080000u
#error Supported internal flash capacities are 512 KiB and 1 MiB
#endif
#if IMAGE_HEADER_SIZE + IMAGE_MAX_PAYLOAD + OTA_SIGNATURE_SIZE > EXT_IMAGE_SLOT_SIZE
#error External image container is too small
#endif
#endif
