#ifndef OTA_UDS_H
#define OTA_UDS_H

#include <stdint.h>
#include "ota/ota_port.h"
#include "ota/boot.h"

#define UDS_SESSION_DEFAULT       0x01u
#define UDS_SESSION_PROGRAMMING   0x02u
#define UDS_SESSION_EXTENDED      0x03u

/* maxNumberOfBlockLength advertised in the 0x34 response */
#define UDS_MAX_DOWNLOAD_BLOCK    512u
#define UDS_MAX_DOWNLOAD_DATA     (UDS_MAX_DOWNLOAD_BLOCK - 2u)
#define UDS_S3_TIMEOUT_MS         5000u
#define UDS_DID_UPDATE_STATUS     0x0200u /* project-specific, read-only */
/* 40 bytes of update state plus 4 bytes of transport/gate state. The extra
 * four were added after a handshake failure on silicon could not be told
 * apart from a closed capability gate by any host-visible signal: the DID
 * answered identically in both cases. */
#define UDS_UPDATE_STATUS_SIZE    44u

/* ---- 0x19 / 0x14 diagnostic trouble codes ---------------------------------
 * RAM only: the table lives in uds_server_t, so it survives S3 timeouts and
 * session changes but is cleared by a power cycle or a reset. Persisting DTCs
 * would mean touching metadata.c (dual copy + seq + CRC + commit-last), which
 * this diagnostic feature does not justify yet.
 *
 * Codes are project-specific U codes (0xC0-0xFF = manufacturer specific) with
 * failure type byte 0x00 = "no sub type information": the code alone names the
 * defect, there is no per-fault sub-classification.
 */
#define UDS_DTC_MAX                      8u
#define UDS_DTC_STATUS_TEST_FAILED           0x01u
#define UDS_DTC_STATUS_TEST_FAILED_THIS_CYCLE 0x02u
#define UDS_DTC_STATUS_CONFIRMED             0x08u
#define UDS_DTC_STATUS_TEST_FAILED_SINCE_CLEAR 0x20u
/* The status bits this ECU actually implements; also the value written when a
 * fault is recorded, and the availabilityMask echoed in the 0x19 response. */
#define UDS_DTC_STATUS_AVAILABILITY (UDS_DTC_STATUS_TEST_FAILED \
        | UDS_DTC_STATUS_TEST_FAILED_THIS_CYCLE \
        | UDS_DTC_STATUS_CONFIRMED \
        | UDS_DTC_STATUS_TEST_FAILED_SINCE_CLEAR)

#define UDS_DTC_EXT_FLASH 0xC10100u /* external NOR erase / program / read */
#define UDS_DTC_PACKAGE   0xC10200u /* downloaded package header / CRC / sig */
#define UDS_DTC_METADATA  0xC10300u /* metadata_store read or write */
#define UDS_DTC_SECURITY  0xC10400u /* 3 consecutive invalid keys -> lockout */

typedef struct {
    uint32_t code;   /* 24-bit, as reported in the 0x19 record */
    uint8_t  status;
} uds_dtc_t;

typedef struct {
    boot_context_t   *boot;
    const ota_port_t *port;

    /* session / security */
    uint8_t  session;
    uint8_t  unlocked;
    uint32_t seed;
    uint8_t  seed_valid;
    uint32_t key_failures;
    uint32_t lockout_started_ms;
    uint8_t  lockout_active;

    /* 0x34 / 0x36 / 0x37 download state */
    uint8_t  dl_active;
    uint8_t  dl_has_previous;
    uint8_t  dl_previous_counter;
    uint32_t dl_last_offset;
    uint16_t dl_last_size;
    uint32_t dl_offset;
    uint32_t dl_size;
    uint32_t dl_written;
    uint8_t  dl_next_counter;

    uint8_t  reset_requested;
    /* Set by handle_session when the programming session was entered and the
     * hook recorded "stop in the loader next reset". Read-only to the tester
     * via DID 0x0200 so a host can confirm the handshake actually landed. */
    uint8_t  boot_request_accepted;
    /* Transport/gate observability, refreshed by the platform diagnostic
     * layer each poll and published as status[40..43]. Core code never reads
     * it; it exists so a host can tell "refused by policy" from "refused
     * because the transport just failed". */
    uint8_t  diag[4];
    /* Called on 10 02. Returns 1 when the request is now durable. A loader
     * leaves this NULL: it is already where the request would send it. */
    int (*request_bootloader)(void *user);
    void *request_bootloader_user;
    uint32_t request_count;
    uint32_t last_request_ms;

    /* 0x19 / 0x14: RAM only, see the constants above */
    uds_dtc_t dtc[UDS_DTC_MAX];
    uint8_t   dtc_count;
    uint8_t   dtc_dropped;  /* distinct codes lost because the table was full */
} uds_server_t;

void uds_init(uds_server_t *s, boot_context_t *boot, const ota_port_t *port);
void uds_poll(uds_server_t *s);

/* Handles one complete request. Returns the response length, 0 means
   "no response" (suppressed positive response). */
uint16_t uds_handle(uds_server_t *s, const uint8_t *req, uint16_t req_len,
                    uint8_t *resp, uint16_t resp_max);

/* Demo security algorithm, exposed so the PC tester tool can reuse it. */
uint32_t uds_calc_key(uint32_t seed);

/* Records (or re-arms) one DTC. Called from the handler failure paths; safe to
   call with a NULL server. Ignored, but counted, when the table is full. */
void uds_dtc_record(uds_server_t *s, uint32_t code);

#endif /* OTA_UDS_H */
