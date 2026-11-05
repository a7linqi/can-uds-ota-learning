#include "ota/boot.h"
#include "ota/image.h"
#include "ota/signature.h"
#include <string.h>

void boot_context_init(boot_context_t *b,flash_port_t internal,
                       flash_port_t external,metadata_store_t *store)
{ b->internal=internal; b->external=external; b->store=store;
  b->last_reason=BOOT_REASON_NO_METADATA; }

static void clear_pending(metadata_t *m)
{
    m->pending_version=0u; m->pending_size=0u; m->pending_crc=0u;
    m->boot_attempts=0u; m->state=UPDATE_IDLE;
}
/* One predicate for "this image may run". CRC32 alone answers "were these
 * bytes damaged in transit", which is not the question a boot decision has to
 * answer: anyone with a debug probe can write a self-consistent image that
 * passes CRC32 and then edit the metadata to match. The ticket answers the
 * question that matters -- "did someone holding the private key ever sign
 * exactly these bytes" -- and every field metadata also knows is compared
 * first, because a valid signature over image A proves nothing about the
 * image now sitting in the app area.
 *
 * version/size/crc are passed in rather than read from the record so the
 * same check serves IDLE (active_*), TRIAL (pending_*) and confirm. */
static int signed_image_ok(boot_context_t *b,uint32_t version,uint32_t size,uint32_t crc)
{
    uint8_t raw[IMAGE_HEADER_SIZE],sig[OTA_SIGNATURE_SIZE];
    image_header_t h;
    int rc;
    if(size<8u || size>IMAGE_MAX_PAYLOAD) {
        b->last_reason=BOOT_REASON_IMAGE_DAMAGED; return 0;
    }
    rc=image_validate_app(b->internal,size,crc);
    if(rc==IMAGE_APP_UNREADABLE) { b->last_reason=BOOT_REASON_UNREADABLE; return 0; }
    if(rc!=IMAGE_APP_VALID) { b->last_reason=BOOT_REASON_IMAGE_DAMAGED; return 0; }
    if(image_ticket_read(b->internal,raw,sig)) {
        b->last_reason=BOOT_REASON_NO_TICKET; return 0;
    }
    image_header_decode(&h,raw);
    if(h.flags!=IMAGE_FLAG_SIGNED) { b->last_reason=BOOT_REASON_NO_TICKET; return 0; }
    if(h.version!=version || h.image_size!=size || h.payload_crc!=crc ||
       h.load_address!=APP_BASE_ADDR) {
        b->last_reason=BOOT_REASON_TICKET_MISMATCH; return 0;
    }
    /* size is what the loader will execute. A ticket that signed a shorter
     * prefix would leave whatever follows it unverified. */
    if(image_signature_verify_split(b->internal,APP_OFFSET,raw,size,sig)) {
        b->last_reason=BOOT_REASON_SIGNATURE; return 0;
    }
    b->last_reason=BOOT_REASON_APP_STARTED;
    return 1;
}
static int active_ok(boot_context_t *b,const metadata_t *m)
{
    if(m->active_size==0u) { b->last_reason=BOOT_REASON_NO_METADATA; return 0; }
    return signed_image_ok(b,m->active_version,m->active_size,m->active_crc);
}
static int pending_ok(boot_context_t *b,const metadata_t *m)
{
    image_header_t h;
    return image_read_header(b->external,DOWNLOAD_OFFSET,&h)==0 &&
           h.version==m->pending_version && h.image_size==m->pending_size &&
           h.payload_crc==m->pending_crc && h.version>m->min_version &&
           image_validate(b->external,DOWNLOAD_OFFSET,0,0)==0;
}
static int backup_ok(boot_context_t *b,const metadata_t *m)
{
    image_header_t h;
    return m->active_size!=0u &&
           image_read_header(b->external,BACKUP_OFFSET,&h)==0 &&
           h.version==m->active_version && h.image_size==m->active_size &&
           h.payload_crc==m->active_crc &&
           image_validate_backup(b->external,BACKUP_OFFSET,0,0)==0;
}
static int create_backup(boot_context_t *b,const metadata_t *m)
{
    image_header_t h;
    uint8_t chunk[256],raw[IMAGE_HEADER_SIZE],sig[OTA_SIGNATURE_SIZE];
    uint32_t off=0u;
    if(!active_ok(b,m) || !b->external.erase || !b->external.write ||
       !b->external.read) return -1;
    /* The live ticket is the only copy of the signature for the image being
     * backed up. Without it the copy could never be verified on restore, and
     * an unverifiable backup is worse than no backup: the restore path would
     * either refuse it (losing the rollback) or accept it (losing secure
     * boot, which is the whole point). */
    if(image_ticket_read(b->internal,raw,sig)) return -1;
    if(b->external.erase(b->external.user,BACKUP_OFFSET,EXT_IMAGE_SLOT_SIZE))
        return -1;
    while(off<m->active_size) {
        uint32_t n=m->active_size-off;
        if(n>sizeof chunk) n=sizeof chunk;
        if(b->internal.read(b->internal.user,APP_OFFSET+off,chunk,n) ||
           b->external.write(b->external.user,BACKUP_OFFSET+IMAGE_HEADER_SIZE+off,chunk,n))
            return -1;
        off+=n;
    }
    memset(&h,0,sizeof h);
    h.magic=IMAGE_MAGIC; h.version=m->active_version; h.image_size=m->active_size;
    h.payload_crc=m->active_crc; h.load_address=APP_BASE_ADDR;
    h.flags=IMAGE_FLAG_SIGNED;
    image_header_finalize(&h); image_header_encode(&h,raw);
    if(b->external.write(b->external.user,BACKUP_OFFSET,raw,sizeof raw) ||
       b->external.write(b->external.user,
                         BACKUP_OFFSET+IMAGE_HEADER_SIZE+m->active_size,
                         sig,OTA_SIGNATURE_SIZE))
        return -1;
    return backup_ok(b,m) ? 0 : -1;
}
static int install(boot_context_t *b,uint32_t source,uint32_t size,uint32_t crc)
{
    uint8_t chunk[256],raw[IMAGE_HEADER_SIZE],sig[OTA_SIGNATURE_SIZE];
    image_header_t h;
    uint32_t off=0u;
    if(!b->internal.erase || !b->internal.write || !b->external.read) return -1;

    /* Read the proof BEFORE erasing anything. Restoring an unsigned backup
     * used to be legal; it is not any more, and installing one would leave an
     * app in internal flash that can never be vouched for. Failing here keeps
     * the previous app intact and leaves the state machine where it was. */
    if(b->external.read(b->external.user,source,raw,sizeof raw)) return -1;
    image_header_decode(&h,raw);
    if(!image_header_ok(&h) || h.flags!=IMAGE_FLAG_SIGNED ||
       h.image_size!=size || h.payload_crc!=crc) return -1;
    if(b->external.read(b->external.user,source+IMAGE_HEADER_SIZE+size,
                        sig,sizeof sig)) return -1;

    if(b->internal.erase(b->internal.user,APP_OFFSET,APP_SIZE)) return -1;
    while(off<size) {
        uint32_t n=size-off,programmed;
        if(n>sizeof chunk) n=sizeof chunk;
        memset(chunk,0xFF,sizeof chunk);
        if(b->external.read(b->external.user,source+IMAGE_HEADER_SIZE+off,chunk,n))
            return -1;
        programmed=(n+3u)&~3u;
        if(b->internal.write(b->internal.user,APP_OFFSET+off,chunk,programmed))
            return -1;
        off+=n;
    }
    if(image_validate_app(b->internal,size,crc)!=IMAGE_APP_VALID) return -1;
    /* The ticket goes in last, and only after the payload reads back clean:
     * the erase above already wiped the old ticket, so an interrupted install
     * leaves an app with no proof, which the next boot refuses to run. */
    return image_ticket_write(b->internal,raw,sig);
}
static int set_state(boot_context_t *b,metadata_t *m,uint32_t state)
{
    metadata_t next=*m;
    next.state=state;
    if(metadata_save(b->store,&next)) return -1;
    *m=next;
    return 0;
}
/* Three-way, because the caller must not treat "cannot read" as "is broken". */
static int probe_active(boot_context_t *b,const metadata_t *m)
{
    int rc;
    if(m->active_size==0u) return IMAGE_APP_CORRUPT;
    rc=image_validate_app(b->internal,m->active_size,m->active_crc);
    if(rc!=IMAGE_APP_VALID) return rc;
    if(signed_image_ok(b,m->active_version,m->active_size,m->active_crc))
        return IMAGE_APP_VALID;
    /* Readable and CRC-clean but unproven is CORRUPT, not UNREADABLE: the
     * bytes are there, they are just not the bytes anyone signed, so no
     * future reset will fix it. Saying CORRUPT is also what unblocks READY --
     * the branch below then clears the active fields and lets a valid package
     * install over it. A storage read failure is still the other case. */
    return b->last_reason==BOOT_REASON_UNREADABLE
           ? IMAGE_APP_UNREADABLE : IMAGE_APP_CORRUPT;
}

boot_target_t boot_select(boot_context_t *b,metadata_t *meta_out)
{
    metadata_t m,next;
    boot_target_t target=BOOT_TARGET_LOADER;
    memset(&m,0,sizeof m);
    if(b) b->last_reason=BOOT_REASON_STATE_MACHINE;
    if(!b || !b->store || metadata_load(b->store,&m)!=1) {
        if(b) b->last_reason=BOOT_REASON_NO_METADATA;
        goto done;
    }

    if(m.state==UPDATE_READY) {
        if(!pending_ok(b,&m)) {
            next=m; clear_pending(&next);
            if(metadata_save(b->store,&next)) goto done;
            m=next;
        } else {
            int keep=probe_active(b,&m);
            if(keep==IMAGE_APP_UNREADABLE) goto done;
            if(m.active_size && keep==IMAGE_APP_VALID) {
                /* While READY, only the backup area can change. The old app
                 * remains intact if a backup write/commit is interrupted. */
                if(create_backup(b,&m)) goto done;
            } else if(m.active_size) {
                /* The installed application no longer validates, so there is
                 * no recovery target left and nothing worth copying. Retiring
                 * it is what lets a good package install over it: keeping the
                 * stale size made create_backup fail on its first check and
                 * wedged READY forever, even with a valid signed package
                 * waiting in the download area.
                 *
                 * min_version has to go with it. metadata.c keeps
                 * min_version == active_version with no active image, so
                 * clearing only the size/crc pair is rejected as insane and
                 * the record keeps its old contents -- the wedge would simply
                 * move one layer down. */
                next=m; next.active_version=0u; next.active_size=0u;
                next.active_crc=0u; next.min_version=0u;
                if(metadata_save(b->store,&next)) goto done;
                m=next;
            }
            if(set_state(b,&m,UPDATE_INSTALLING)) goto done;
        }
    }
    if(m.state==UPDATE_INSTALLING) {
        if(m.active_size && !backup_ok(b,&m)) goto done;
        if(!pending_ok(b,&m)) {
            if(m.active_size==0u) {
                next=m; clear_pending(&next);
                if(metadata_save(b->store,&next)==0) m=next;
                goto done;
            }
            if(!backup_ok(b,&m) || set_state(b,&m,UPDATE_RESTORING)) goto done;
        } else {
            if(install(b,DOWNLOAD_OFFSET,m.pending_size,m.pending_crc)) goto done;
            if(set_state(b,&m,UPDATE_TRIAL)) goto done;
        }
    }
    if(m.state==UPDATE_TRIAL) {
        if(m.boot_attempts>=MAX_BOOT_ATTEMPTS ||
           !signed_image_ok(b,m.pending_version,m.pending_size,m.pending_crc)) {
            if(m.active_size==0u) {
                next=m; clear_pending(&next);
                if(metadata_save(b->store,&next)==0) m=next;
                goto done;
            }
            if(!backup_ok(b,&m) || set_state(b,&m,UPDATE_RESTORING)) goto done;
        } else {
            next=m; next.boot_attempts++;
            /* A failed counter commit MUST NOT launch an untracked trial. */
            if(metadata_save(b->store,&next)) goto done;
            m=next; target=BOOT_TARGET_APP; goto done;
        }
    }
    if(m.state==UPDATE_RESTORING) {
        /* A reset anywhere in restoration repeats this entire copy. */
        if(!backup_ok(b,&m) ||
           install(b,BACKUP_OFFSET,m.active_size,m.active_crc)) goto done;
        next=m; clear_pending(&next);
        if(metadata_save(b->store,&next)) goto done;
        m=next;
    }
    if((m.state==UPDATE_IDLE || m.state==UPDATE_DOWNLOADING) && active_ok(b,&m)) {
        target=BOOT_TARGET_APP;
        b->last_reason=BOOT_REASON_APP_STARTED;
    }
done:
    if(meta_out) *meta_out=m;
    return target;
}
int boot_confirm(boot_context_t *b)
{
    metadata_t m;
    if(!b || !b->store || metadata_load(b->store,&m)!=1 ||
       m.state!=UPDATE_TRIAL || m.boot_attempts==0u ||
       !signed_image_ok(b,m.pending_version,m.pending_size,m.pending_crc)) return -1;
    m.active_version=m.pending_version; m.active_size=m.pending_size;
    m.active_crc=m.pending_crc; m.min_version=m.active_version;
    clear_pending(&m);
    return metadata_save(b->store,&m);
}
int boot_request_loader(boot_context_t *b)
{
    metadata_t m;
    if(!b || !b->store || metadata_load(b->store,&m)!=1) return -1;
    if(m.boot_request==1u) return 0;
    m.boot_request=1u;
    return metadata_save(b->store,&m);
}
int boot_consume_boot_request(boot_context_t *b)
{
    metadata_t m;
    if(!b || !b->store) return 1;
    if(metadata_load(b->store,&m)!=1) {
        /* The request cannot be read, so it cannot be ruled out either.
         * boot_select already treats this same failure as "stay in the
         * loader", and stopping is the only recoverable answer: booting the
         * application after a tester asked to be let in leaves no way back
         * without physical access. Measured on silicon, this failure mode
         * silently swallowed the handshake and the board started the App. */
        return 1;
    }
    if(m.boot_request==0u) return 0;
    m.boot_request=0u;
    (void)metadata_save(b->store,&m);
    return 1;
}
int boot_accept_app_health(boot_context_t *b)
{
    metadata_t m;
    if(!b || !b->store || metadata_load(b->store,&m)!=1) return -1;
    if(m.state==UPDATE_TRIAL) return boot_confirm(b);
    if(m.state==UPDATE_IDLE || m.state==UPDATE_DOWNLOADING) return active_ok(b,&m)?0:-1;
    return -1;
}
