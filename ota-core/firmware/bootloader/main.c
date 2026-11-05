#include "ota_stm32.h"
static ota_stm32_ports_t storage;
static ota_stm32_diag_t diagnostics;
#if OTA_ENABLE_WATCHDOG
static watchdog_policy_t watchdog;
static int watchdog_started;
#endif
int ota_firmware_watchdog_service(void)
{
#if OTA_ENABLE_WATCHDOG
    if(!watchdog_started) return 0;
    if(watchdog.phase==WATCHDOG_FAILED) return -1;
    if(watchdog_policy_should_feed(&watchdog,ota_bsp_now_ms(),1) && ota_bsp_watchdog_reload()) {
        watchdog_policy_fail(&watchdog); return -1;
    }
#endif
    return 0;
}
int ota_firmware_service(void) { return ota_stm32_storage_service(&storage); }
/* Call from the board's CAN FIFO0 callback after HAL has read one frame. */
void ota_bootloader_can_rx(const can_frame_t *frame)
{ (void)ota_stm32_can_rx_isr(&diagnostics.can,frame); }
/* Call from the board's USART1 RX-complete callback in UART bench builds. */
void ota_user_uart_rx(uint8_t byte)
{
#if OTA_DIAG_UART
    (void)ota_stm32_uart_rx_isr(&diagnostics.uart,byte);
#else
    (void)byte;
#endif
}
int main(void)
{
    ota_board_config_t config;
    uint32_t last;
    int logged_error=0;
    ota_arch_irq_enable();
    if(ota_bsp_init(&config) || !config.board_verified) {
        ota_bsp_log("BSP unconfigured: no erase, install or handoff");
        for(;;) ota_bsp_idle();
    }
#if OTA_ENABLE_WATCHDOG
    watchdog_policy_init(&watchdog,0,ota_bsp_now_ms(),0u);
    if(ota_bsp_watchdog_start()) {
        ota_bsp_log("IWDG start failed: no install or handoff");
        for(;;) ota_bsp_idle();
    }
    watchdog_started=1;
#endif
    ota_stm32_ports_init(&storage,&config);
#if OTA_ENABLE_BOOT_HANDOFF
    /* A first install erases the 1 MiB backup slot in the NOR and 960 KiB of
     * internal flash, so these lines are the only sign of life for ~30 s. */
    if(!storage.storage_ready)
        ota_bsp_log("loader: storage not ready, staying in the loader");
    else if(boot_consume_boot_request(&storage.boot))
        ota_bsp_log("loader: stop requested by the application");
    else if(!ota_bsp_force_loader()) {
        ota_bsp_log("loader: selecting boot target");
        if(boot_select(&storage.boot,0)==BOOT_TARGET_APP) {
            ota_bsp_log("loader: handing off to the app at 08010000");
            (void)ota_stm32_jump_to_app(&storage);
        }
        /* "Staying in the loader" is the same sentence for six very different
         * situations. Without the reason a board with no debugger attached
         * cannot be told apart from a board that is refusing an unsigned
         * image, and the only way to find out is to guess. */
        switch(storage.boot.last_reason) {
        case BOOT_REASON_NO_METADATA:
            ota_bsp_log("loader: no usable metadata record");
            break;
        case BOOT_REASON_IMAGE_DAMAGED:
            ota_bsp_log("loader: installed app failed its CRC or vectors");
            break;
        case BOOT_REASON_NO_TICKET:
            ota_bsp_log("loader: installed app has no signed ticket");
            break;
        case BOOT_REASON_TICKET_MISMATCH:
            ota_bsp_log("loader: ticket describes a different image");
            break;
        case BOOT_REASON_SIGNATURE:
            ota_bsp_log("loader: ticket signature does not verify");
            break;
        case BOOT_REASON_UNREADABLE:
            ota_bsp_log("loader: internal flash could not be read");
            break;
        default:
            ota_bsp_log("loader: update state machine has work to do");
            break;
        }
        ota_bsp_log("loader: staying in the loader");
    }
#endif
    /* Bring-up first supports version reads even without storage drivers. */
#if OTA_DIAG_UART
    if(ota_stm32_diag_init_uart(&diagnostics,&storage)) {
        ota_bsp_log("UART diagnostics start failed; verify USART1 wiring");
        for(;;) ota_bsp_idle();
    }
    /* Binary transport owns USART1; text logging is suppressed by the BSP. */
#else
    ota_stm32_diag_init(&diagnostics,&storage);
    if(ota_bsp_can_start()) {
        ota_bsp_log("CAN start failed; verify board configuration");
        for(;;) ota_bsp_idle();
    }
    ota_bsp_log("loader diagnostics: request 7E0, response 7E8");
#endif
    last=ota_bsp_now_ms();
    for(;;) {
        if(ota_firmware_service()) {
            ota_bsp_log("background service failed: awaiting watchdog/reset");
            for(;;) ota_bsp_idle();
        }
        if(ota_stm32_diag_poll(&diagnostics)<0 && !logged_error) {
            ota_bsp_log("diagnostic transport error; inspect CAN state");
            logged_error=1;
        }
        if((uint32_t)(ota_bsp_now_ms()-last)>=500u) {
            last=ota_bsp_now_ms(); ota_bsp_heartbeat();
        }
        ota_bsp_idle();
    }
}
