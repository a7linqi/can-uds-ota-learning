#include "explorer_reference.h"
void SysTick_Handler(void) { HAL_IncTick();HAL_SYSTICK_IRQHandler(); }
void CAN1_RX0_IRQHandler(void) { HAL_CAN_IRQHandler(ota_user_can()); }
void USART1_IRQHandler(void) { HAL_UART_IRQHandler(ota_user_uart()); }
void HAL_CAN_RxFifo0MsgPendingCallback(CAN_HandleTypeDef *can)
{ ota_hal_can_fifo0_callback(can); }
void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{ ota_hal_uart_rx_complete(huart); }
void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{ ota_hal_uart_error(huart); }
