#include "ota_stm32.h"
#include <string.h>
#define PENDING_INTERVAL_MS 1000u
#define SERVICE_GAP_LIMIT_MS 1000u
#define INITIAL_TX_TIMEOUT_MS 1000u
static int long_service(uint8_t sid)
{ return sid==0x34u || sid==0x36u || sid==0x37u || sid==0x31u; }
static int status_read(const uint8_t *request,uint16_t length)
{
    uint16_t i;
    if(length<3u || request[0]!=0x22u || ((length-1u)&1u)) return 0;
    for(i=1u;i<length;i=(uint16_t)(i+2u))
        if(request[i]==(UDS_DID_UPDATE_STATUS>>8) &&
           request[i+1u]==(UDS_DID_UPDATE_STATUS&255u)) return 1;
    return 0;
}
static int send_pending(ota_stm32_diag_t *d)
{
    uint8_t response[3]={0x7Fu,0u,0x78u};
    response[1]=d->request[0];
    if(isotp_send(&d->link,response,sizeof response)) return -1;
    d->pending_last_ms=ota_bsp_now_ms(); d->pending_count++;
    d->pending_in_flight=1u;
    return 0;
}
static int job_service(void *user)
{
    ota_stm32_diag_t *d=(ota_stm32_diag_t *)user;
    uint32_t now,gap;
    if(!d->job_running) return 0;
    if(d->job_error) return -1;
    now=ota_bsp_now_ms(); gap=now-d->service_last_ms;
    if(gap>d->max_service_gap_ms) d->max_service_gap_ms=gap;
    d->service_last_ms=now;
    if(gap>SERVICE_GAP_LIMIT_MS) {
        d->service_deadline_misses++; goto failed;
    }
    /* S3 does not expire while this server is processing its current job. */
    d->uds.last_request_ms=now;
    if(!d->io.poll || d->io.poll(d->io.port.user)) goto failed;
    if(d->pending_in_flight) {
        if(d->io.idle && d->io.idle(d->io.port.user)) d->pending_in_flight=0u;
        else if((uint32_t)(now-d->pending_last_ms)>INITIAL_TX_TIMEOUT_MS) goto failed;
    }
    if(!d->pending_in_flight && (uint32_t)(now-d->pending_last_ms)>=PENDING_INTERVAL_MS) {
        if(send_pending(d) || (d->io.poll && d->io.poll(d->io.port.user))) goto failed;
    }
    return 0;
failed:
    d->job_error=1u; d->programming_allowed=0u;
    return -1;
}
/* 10 02 only becomes a loader handoff when an application is the one asking.
 * The loader is already where that request would send it; letting it write the
 * flag would re-arm the stop on every programming session and the finished
 * install could never be selected. */
static int diag_request_bootloader(void *user)
{
    if(!ota_bsp_is_application()) return 0;
    return boot_request_loader((boot_context_t *)user)==0;
}
static void diag_common(ota_stm32_diag_t *d,ota_stm32_ports_t *p)
{
    d->storage=p;
    isotp_init(&d->link,&d->io.port,0x7E8u,0x7E0u);
    isotp_set_flow(&d->link,8u,0u);
    uds_init(&d->uds,&p->boot,&d->io.port);
    d->uds.request_bootloader=diag_request_bootloader;
    d->uds.request_bootloader_user=&p->boot;
    d->programming_allowed=(uint8_t)(OTA_ENABLE_FLASHING && p->storage_ready);
    p->service=job_service; p->service_user=d;
}
int ota_stm32_diag_init(ota_stm32_diag_t *d,ota_stm32_ports_t *p)
{
    memset(d,0,sizeof *d);
    ota_stm32_can_init(&d->can);
    d->io=ota_stm32_can_link(&d->can);
    diag_common(d,p);
    return 0;
}
int ota_stm32_diag_init_uart(ota_stm32_diag_t *d,ota_stm32_ports_t *p)
{
    memset(d,0,sizeof *d);
    ota_stm32_uart_init(&d->uart);
    d->io=ota_stm32_uart_link(&d->uart);
    diag_common(d,p);
    ota_bsp_uart_binary(1);
    if(ota_bsp_uart_start()) {
        ota_bsp_uart_binary(0);
        return -1;
    }
    return 0;
}
int ota_stm32_diag_poll(ota_stm32_diag_t *d)
{
    can_frame_t frame;
    int event;
    uint32_t accepted=0u;
    d->uds.diag[0]=d->programming_allowed;
    d->uds.diag[1]=d->job_error;
    d->uds.diag[2]=d->pending_in_flight;
    d->uds.diag[3]=(uint8_t)((ota_bsp_is_application()?0x80u:0u) |
                             (d->service_deadline_misses & 0x7Fu));
    if(!d->io.poll || d->io.poll(d->io.port.user)) return -1;
    /* Limit work so a noisy RX source cannot starve timeouts/TX. */
    while(accepted++<OTA_RX_QUEUE && d->io.pop && d->io.pop(d->io.port.user,&frame)) {
        if(frame.id==0x7E0u && !frame.extended) isotp_on_frame(&d->link,&frame);
    }
    uds_poll(&d->uds);
    event=isotp_poll(&d->link);
    if(event<0) { d->request_pending=0u; return event; }
    if(event==1) {
        if(!d->request_pending) {
            if(isotp_read(&d->link,d->request,sizeof d->request,&d->request_len)==1)
                d->request_pending=1u;
        } else {
            /* One tester/one outstanding request. Do not overwrite it. */
            uint16_t ignored;
            (void)isotp_read(&d->link,d->response,sizeof d->response,&ignored);
            d->requests_dropped++;
        }
    }
    if(d->request_pending && d->link.tx_state==ISOTP_IDLE && !d->reset_pending) {
        uint16_t length;
        uint8_t sid=d->request[0];
        int deferred=(d->programming_allowed && long_service(sid)) ||
                     (d->storage->storage_ready && status_read(d->request,d->request_len));
        if(deferred && !d->long_announced) {
            if(send_pending(d)) return -1;
            d->long_announced=1u;
            /* Return to the scheduler: only start storage after the pending
             * response has left both the queue and hardware mailboxes. */
            goto finish_tx;
        }
        if(deferred && (!d->io.idle || !d->io.idle(d->io.port.user))) {
            if((uint32_t)(ota_bsp_now_ms()-d->pending_last_ms)>INITIAL_TX_TIMEOUT_MS) {
                d->request_pending=0u; d->long_announced=0u;
                d->programming_allowed=0u; return -1;
            }
            goto finish_tx;
        }
        d->job_error=0u; d->pending_in_flight=0u;
        /* Two different reasons can close the gate, and they look identical on
         * the wire, so keep them apart here:
         *   - built closed: the application is compiled with
         *     OTA_ENABLE_FLASHING=0 and may only hand the next reset over.
         *     NRC 0x22 for 0x34 there is the correct answer, not a defect.
         *   - closed by failure: a loader job missed its deadline or lost the
         *     link. That must not turn into a fuse, so a new programming
         *     session re-evaluates the gate from the build switch and storage
         *     readiness instead of staying shut until a power cycle.
         * The status DID carries the current gate in diag[0] and the role in
         * diag[3] bit 7 so a host can tell the two apart without guessing. */
        if(sid==0x10u && d->request_len>1u &&
           (d->request[1]&0x7Fu)==UDS_SESSION_PROGRAMMING)
            d->programming_allowed=(uint8_t)(OTA_ENABLE_FLASHING &&
                                             d->storage->storage_ready);
        if(!d->programming_allowed && (sid==0x34u || sid==0x36u || sid==0x37u || sid==0x31u)) {
            d->response[0]=0x7Fu; d->response[1]=sid; d->response[2]=0x22u; length=3u;
        } else {
            d->job_running=(uint8_t)deferred; d->job_error=0u;
            d->pending_in_flight=0u;
            d->service_last_ms=ota_bsp_now_ms();
            /* Once 0x78 was sent, the tester needs a final response even if
             * RoutineControl originally suppressed its positive response. */
            if(deferred && sid==0x31u && d->request_len>1u) d->request[1]&=0x7Fu;
            length=uds_handle(&d->uds,d->request,d->request_len,d->response,sizeof d->response);
            d->job_running=0u; d->uds.last_request_ms=ota_bsp_now_ms();
        }
        d->request_pending=0u;
        d->long_announced=0u;
        if(d->job_error) return -1;
        if(length && isotp_send(&d->link,d->response,length)!=ISOTP_OK) return -1;
        if(d->uds.reset_requested) d->reset_pending=1u;
    }
finish_tx:
    if(!d->io.poll || d->io.poll(d->io.port.user)) return -1;
    if(d->reset_pending && !isotp_busy(&d->link) &&
       d->io.idle && d->io.idle(d->io.port.user)) {
        d->reset_pending=0u; d->uds.reset_requested=0u; ota_bsp_reset();
    }
    return 0;
}
