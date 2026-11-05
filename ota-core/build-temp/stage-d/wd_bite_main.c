/* Temporary: does the IWDG actually reset this board?
 *
 * Starts exactly the watchdog a product build would start (HAL IWDG,
 * prescaler 256, reload 4095 -> ~32.8 s at the nominal 32 kHz LSI), then
 * never reloads it. No storage work, no UDS: just a loop.
 *
 * Every reset prints one banner, so the time between two consecutive
 * "wd_bite: watchdog started" lines is the measured timeout of the real
 * LSI on this chip -- not the datasheet number.
 *
 * Links at 0x08000000 and replaces the loader. Reflash the real loader and
 * app afterwards. Never shipped.
 */
#include "ota_stm32.h"
#include <string.h>

int ota_firmware_service(void) { return 0; }
int ota_firmware_watchdog_service(void) { return 0; }
void ota_bootloader_can_rx(const can_frame_t *frame) { (void)frame; }
void ota_user_uart_rx(uint8_t byte) { (void)byte; }

int main(void)
{
    ota_board_config_t config;
    ota_arch_irq_enable();
    if (ota_bsp_init(&config) || !config.board_verified) {
        ota_bsp_log("wd_bite: BSP init failed; watchdog not started");
        for (;;) ota_bsp_idle();
    }
    if (ota_bsp_watchdog_start()) {
        ota_bsp_log("wd_bite: watchdog start FAILED -- nothing to measure");
        for (;;) ota_bsp_idle();
    }
    ota_bsp_log("wd_bite: watchdog started, no reload from here");
    for (;;) ota_bsp_idle();
}
