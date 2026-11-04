#ifndef OTA_STM32_H
#define OTA_STM32_H
#include "ota_board.h"
#include "ota/boot.h"
#include "ota/can_uart.h"
#include "ota/isotp.h"
#include "ota/uds.h"
#include "ota/watchdog_policy.h"

#ifndef OTA_ENABLE_FLASHING
#define OTA_ENABLE_FLASHING 0
#endif
#ifndef OTA_ENABLE_BOOT_HANDOFF
#define OTA_ENABLE_BOOT_HANDOFF 0
#endif
#ifndef OTA_ENABLE_WATCHDOG
#define OTA_ENABLE_WATCHDOG 0
#endif
/* Bench transport selector: 0 = classic CAN, 1 = CAN-over-UART. */
#ifndef OTA_DIAG_UART
#define OTA_DIAG_UART 0
#endif
#define OTA_APP_HEALTH_DEADLINE_MS 10000u
typedef struct {
    ota_board_config_t config;
    uint8_t storage_ready;
    flash_port_t internal,external;
    eeprom_port_t eeprom;
    metadata_store_t store;
    boot_context_t boot;
    int (*service)(void *user);
    void *service_user;
} ota_stm32_ports_t;
void ota_stm32_ports_init(ota_stm32_ports_t *p,const ota_board_config_t *cfg);
/* Cooperative TX/pending service; never dispatches another UDS request. */
int ota_stm32_storage_service(ota_stm32_ports_t *p);
flash_port_t ota_stm32_internal_port(ota_stm32_ports_t *p);
flash_port_t ota_stm32_nor_port(ota_stm32_ports_t *p);
eeprom_port_t ota_stm32_eeprom_port(ota_stm32_ports_t *p);

#define OTA_RX_QUEUE 32u
#define OTA_TX_QUEUE 8u
typedef struct {
    can_frame_t rx[OTA_RX_QUEUE],tx[OTA_TX_QUEUE];
    volatile uint32_t rx_head,rx_tail;
    uint32_t tx_head,tx_tail,rx_dropped;
    int tx_error;
} ota_stm32_can_t;
void ota_stm32_can_init(ota_stm32_can_t *c);
/* ISR only enqueues; never parse UDS or access storage here. */
int ota_stm32_can_rx_isr(ota_stm32_can_t *c,const can_frame_t *frame);
int ota_stm32_can_pop(ota_stm32_can_t *c,can_frame_t *frame);
int ota_stm32_can_poll(ota_stm32_can_t *c);
int ota_stm32_can_idle(const ota_stm32_can_t *c);
ota_port_t ota_stm32_can_port(ota_stm32_can_t *c);

/* Transport-neutral link used by the diagnostic task. The underlying queue
 * is still a classic-CAN frame queue: UART carries encoded CAN frames, so
 * ISO-TP/UDS/boot logic do not change when the physical link changes. */
typedef struct {
    ota_port_t port;
    int (*poll)(void *user);
    int (*pop)(void *user,can_frame_t *frame);
    int (*idle)(const void *user);
} ota_stm32_link_t;
ota_stm32_link_t ota_stm32_can_link(ota_stm32_can_t *c);

typedef struct {
    can_frame_t rx[OTA_RX_QUEUE],tx[OTA_TX_QUEUE];
    volatile uint32_t rx_head,rx_tail;
    uint32_t tx_head,tx_tail,rx_dropped;
    int tx_error;
    can_uart_decoder_t decoder;
} ota_stm32_uart_t;
void ota_stm32_uart_init(ota_stm32_uart_t *u);
/* Feed one received UART byte; enqueues only complete valid frames. */
int ota_stm32_uart_rx_isr(ota_stm32_uart_t *u,uint8_t byte);
int ota_stm32_uart_pop(ota_stm32_uart_t *u,can_frame_t *frame);
int ota_stm32_uart_poll(ota_stm32_uart_t *u);
int ota_stm32_uart_idle(const ota_stm32_uart_t *u);
ota_port_t ota_stm32_uart_port(ota_stm32_uart_t *u);
ota_stm32_link_t ota_stm32_uart_link(ota_stm32_uart_t *u);

typedef struct {
    ota_stm32_can_t can;
    ota_stm32_uart_t uart;
    ota_stm32_link_t io;
    isotp_link_t link;
    uds_server_t uds;
    ota_stm32_ports_t *storage;
    uint8_t request[ISOTP_MAX_PAYLOAD],response[512];
    uint16_t request_len;
    uint8_t request_pending,reset_pending,programming_allowed;
    uint32_t requests_dropped;
    uint8_t long_announced,job_running,job_error,pending_in_flight;
    uint32_t pending_last_ms,service_last_ms,pending_count;
    uint32_t max_service_gap_ms,service_deadline_misses;
} ota_stm32_diag_t;
/* Returns 0 on success; the UART variant also starts its RX interrupt. */
int ota_stm32_diag_init(ota_stm32_diag_t *d,ota_stm32_ports_t *p);
int ota_stm32_diag_init_uart(ota_stm32_diag_t *d,ota_stm32_ports_t *p);
int ota_stm32_diag_poll(ota_stm32_diag_t *d);

/* Hardware-only primitives; host adapter tests supply mocks. */
uint32_t ota_arch_irq_save(void);
void ota_arch_irq_restore(uint32_t saved);
void ota_arch_irq_enable(void);
void ota_arch_wait(void);
void ota_arch_reset(void);
int ota_arch_can_handoff(void);
void ota_arch_handoff(uint32_t stack,uint32_t reset);
int ota_stm32_app_vectors(ota_stm32_ports_t *p,uint32_t *stack,uint32_t *entry);
int ota_stm32_jump_to_app(ota_stm32_ports_t *p);
#endif
