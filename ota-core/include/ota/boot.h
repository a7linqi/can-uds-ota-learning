#ifndef OTA_BOOT_H
#define OTA_BOOT_H
#include "ota/metadata.h"

/* Why the last boot_select() did not run the application. "Staying in the
 * loader" alone is not a diagnosis: an erased ticket, a forged image and a
 * broken state machine all land there, and on a board with no debugger
 * attached the reason is the only thing that separates them. */
typedef enum {
    BOOT_REASON_APP_STARTED = 0,
    BOOT_REASON_NO_METADATA,
    BOOT_REASON_STATE_MACHINE,   /* READY/INSTALLING/TRIAL asked to stay */
    BOOT_REASON_IMAGE_DAMAGED,   /* CRC32 or vector table failed */
    BOOT_REASON_NO_TICKET,       /* app is intact but nothing signed it */
    BOOT_REASON_TICKET_MISMATCH, /* ticket describes a different image */
    BOOT_REASON_SIGNATURE,       /* ticket present, signature does not hold */
    BOOT_REASON_UNREADABLE       /* internal flash could not be read */
} boot_reason_t;

typedef struct {
    flash_port_t internal;
    flash_port_t external;
    metadata_store_t *store;
    /* Output of the last boot_select(); read-only to callers. */
    uint8_t last_reason;
} boot_context_t;
typedef enum { BOOT_TARGET_LOADER = 0, BOOT_TARGET_APP = 1 } boot_target_t;
void boot_context_init(boot_context_t *b, flash_port_t internal,
                       flash_port_t external, metadata_store_t *store);
/* Once per reset. Never jumps itself. Any failed persistence/copy step
 * stays in the loader. INSTALLING/RESTORING always restart a full copy. */
boot_target_t boot_select(boot_context_t *b, metadata_t *meta_out);
/* Application health hook only, after a real trial boot. There is no UDS
 * command to impersonate this hook. Validates installed bytes again. */
int boot_confirm(boot_context_t *b);
/* Trusted running-App health hook: confirm TRIAL, or validate the existing
 * active app in IDLE/DOWNLOADING without modifying download metadata. */
int boot_accept_app_health(boot_context_t *b);
/* Application side of the programming-session handshake. The running App has
 * no way to erase itself and no flash-write authorization, so it can only ask
 * the next reset to stop in the loader. Persisted rather than kept in RAM: the
 * request has to survive the reset that follows, and the user may also pull
 * power between asking and rebooting. */
int boot_request_loader(boot_context_t *b);
/* Loader side: returns 1 when this reset must stay in the loader, and clears
 * the request. A failed clear still returns 1 -- honouring the request twice
 * is harmless, jumping into an App the tester asked to leave is not. */
int boot_consume_boot_request(boot_context_t *b);
#endif
