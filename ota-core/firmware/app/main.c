#include "ota_stm32.h"
static ota_stm32_ports_t storage;
/* The application keeps a diagnostic transport for one reason: entering
 * programming session 02 is how it hands the next reset to the loader. It has
 * no flash-write authorization, so 34/36/37/31 still answer NRC 0x22 here. */
static ota_stm32_diag_t diagnostics;
static int diagnostics_ready;
/* App storage/ACK callbacks must not keep an unhealthy App alive. Only the
 * main-loop health decision below is allowed to refresh the watchdog. */
int ota_firmware_watchdog_service(void) { return 0; }
int ota_firmware_service(void) { return ota_stm32_storage_service(&storage); }
void ota_user_uart_rx(uint8_t byte)
{
    /* Same ISR hand-off the bootloader uses. Without it the application's UDS
     * server and the diagnostic transport are wired to each other and to
     * nothing else: 10 02 would never arrive, and the loader handshake would
     * look like a silent board rather than a missing queue push. */
    if(diagnostics_ready) (void)ota_stm32_uart_rx_isr(&diagnostics.uart,byte);
}
/* The board binding fan-outs the CAN FIFO to whichever main is linked in. The
 * application has no diagnostic transport, but the hook still has to resolve. */
void ota_bootloader_can_rx(const can_frame_t *frame) { (void)frame; }
int main(void)
{
    ota_board_config_t config;
    uint32_t last;
    int confirmed=0;
#if OTA_ENABLE_WATCHDOG
    watchdog_policy_t guard;
#endif
    /* ST startup initialized data/BSS and SystemInit set VTOR to __Vectors.
     * The loader deliberately handed over with interrupts still masked. */
    ota_arch_irq_enable();
    if(ota_bsp_init(&config) || !config.board_verified) {
        for(;;) ota_bsp_idle();
    }
#if OTA_ENABLE_WATCHDOG
    if(ota_bsp_watchdog_start()) for(;;) ota_bsp_idle();
    watchdog_policy_init(&guard,1,ota_bsp_now_ms(),OTA_APP_HEALTH_DEADLINE_MS);
#endif
    ota_stm32_ports_init(&storage,&config);
    ota_bsp_log("app at 08010000: awaiting real health checks");
    if(ota_stm32_diag_init_uart(&diagnostics,&storage)==0) diagnostics_ready=1;
    else ota_bsp_log("app: UART diagnostics unavailable, health checks only");
    last=ota_bsp_now_ms();
    for(;;) {
        /* A transport error must not take the application down: it would turn
         * a wiring problem into an unbootable board. */
        if(diagnostics_ready) (void)ota_stm32_diag_poll(&diagnostics);
        if((uint32_t)(ota_bsp_now_ms()-last)>=500u) {
            last=ota_bsp_now_ms(); ota_bsp_heartbeat();
#if OTA_ENABLE_WATCHDOG
            if(guard.phase!=WATCHDOG_FAILED &&
               (confirmed || watchdog_policy_may_confirm(&guard,ota_bsp_now_ms()))) {
                int healthy=ota_bsp_app_healthy();
                if(!confirmed && healthy && storage.storage_ready &&
                   watchdog_policy_may_confirm(&guard,ota_bsp_now_ms()) &&
                   boot_accept_app_health(&storage.boot)==0 && watchdog_policy_confirmed(&guard)==0)
                    confirmed=1;
                if(watchdog_policy_should_feed(&guard,ota_bsp_now_ms(),healthy) && ota_bsp_watchdog_reload())
                    watchdog_policy_fail(&guard);
            }
#else
            if(!confirmed && storage.storage_ready && ota_bsp_app_healthy() &&
               boot_accept_app_health(&storage.boot)==0) confirmed=1;
#endif
        }
        ota_bsp_idle();
    }
}
