#include "ota_stm32.h"
#include <string.h>

static int valid(const can_frame_t *f)
{
    return f && f->dlc > 0u && f->dlc <= 8u &&
           f->id <= (f->extended ? 0x1FFFFFFFu : 0x7FFu);
}

void ota_stm32_uart_init(ota_stm32_uart_t *u)
{
    memset(u, 0, sizeof *u);
    can_uart_decoder_init(&u->decoder);
}

int ota_stm32_uart_rx_isr(ota_stm32_uart_t *u, uint8_t byte)
{
    can_frame_t f;
    uint32_t irq, next;

    if (u == 0) {
        return -1;
    }
    if (can_uart_decoder_feed(&u->decoder, byte, &f) != 1) {
        return 0; /* partial frame or rejected bytes */
    }
    irq = ota_arch_irq_save();
    next = (u->rx_head + 1u) % OTA_RX_QUEUE;
    if (next == u->rx_tail) {
        u->rx_dropped++;
        ota_arch_irq_restore(irq);
        return -1;
    }
    u->rx[u->rx_head] = f;
    u->rx_head = next;
    ota_arch_irq_restore(irq);
    return 0;
}

int ota_stm32_uart_pop(ota_stm32_uart_t *u, can_frame_t *f)
{
    uint32_t irq;
    if (u == 0 || f == 0) {
        return 0;
    }
    irq = ota_arch_irq_save();
    if (u->rx_head == u->rx_tail) {
        ota_arch_irq_restore(irq);
        return 0;
    }
    *f = u->rx[u->rx_tail];
    u->rx_tail = (u->rx_tail + 1u) % OTA_RX_QUEUE;
    ota_arch_irq_restore(irq);
    return 1;
}

static int send_frame(void *user, const can_frame_t *f)
{
    ota_stm32_uart_t *u = (ota_stm32_uart_t *)user;
    uint32_t next;

    if (u == 0 || !valid(f) || u->tx_error) {
        return -1;
    }
    next = (u->tx_head + 1u) % OTA_TX_QUEUE;
    if (next == u->tx_tail) {
        return -1;
    }
    u->tx[u->tx_head] = *f;
    u->tx_head = next;
    return 0;
}

static uint32_t now_ms(void *user)
{
    (void)user;
    return ota_bsp_now_ms();
}

ota_port_t ota_stm32_uart_port(ota_stm32_uart_t *u)
{
    ota_port_t p;
    p.can_send = send_frame;
    p.now_ms = now_ms;
    p.user = u;
    return p;
}

int ota_stm32_uart_poll(ota_stm32_uart_t *u)
{
    uint8_t raw[CAN_UART_MAX_FRAME];
    uint32_t n;

    if (u == 0 || u->tx_error) {
        return u ? -1 : 0;
    }
    if (u->tx_tail != u->tx_head) {
        n = can_uart_encode(&u->tx[u->tx_tail], raw, sizeof raw);
        if (n == 0u || ota_bsp_uart_write(raw, n)) {
            u->tx_error = 1;
            return -1;
        }
        u->tx_tail = (u->tx_tail + 1u) % OTA_TX_QUEUE;
    }
    return 0;
}

int ota_stm32_uart_idle(const ota_stm32_uart_t *u)
{
    return u && !u->tx_error && u->tx_head == u->tx_tail &&
           ota_bsp_uart_tx_idle() == 1;
}

static int link_poll(void *u) { return ota_stm32_uart_poll((ota_stm32_uart_t *)u); }
static int link_pop(void *u, can_frame_t *f) { return ota_stm32_uart_pop((ota_stm32_uart_t *)u, f); }
static int link_idle(const void *u) { return ota_stm32_uart_idle((const ota_stm32_uart_t *)u); }

ota_stm32_link_t ota_stm32_uart_link(ota_stm32_uart_t *u)
{
    ota_stm32_link_t l;
    l.port = ota_stm32_uart_port(u);
    l.poll = link_poll;
    l.pop = link_pop;
    l.idle = link_idle;
    return l;
}
