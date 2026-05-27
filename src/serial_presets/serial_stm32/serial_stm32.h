#ifndef SERIAL_STM32_H
#define SERIAL_STM32_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>

/* CubeMX-generated main.h pulls in the family HAL header (stm32fXxx_hal.h, etc.)
 * If you don't use main.h, define RB9704_STM32_HAL_HEADER to the correct header
 * before including rockblock_9704.h, e.g.:
 *     #define RB9704_STM32_HAL_HEADER "stm32f4xx_hal.h"
 */
#ifndef RB9704_STM32_HAL_HEADER
#define RB9704_STM32_HAL_HEADER "main.h"
#endif
#include RB9704_STM32_HAL_HEADER

/**
 * @def RB9704_RX_RING_SIZE
 * @brief Size of the interrupt-driven RX ring buffer, in bytes.
 *
 * MUST be a power of two. Defaults to 512. The ring buffer absorbs bytes
 * between rbPoll() calls; if you can guarantee rbPoll() runs every <10 ms,
 * 256 is sufficient. Reduce only if RAM is tight.
 */
#ifndef RB9704_RX_RING_SIZE
#define RB9704_RX_RING_SIZE 512U
#endif

#if (RB9704_RX_RING_SIZE & (RB9704_RX_RING_SIZE - 1U)) != 0U
#error "RB9704_RX_RING_SIZE must be a power of two"
#endif

/**
 * @def RB9704_TX_TIMEOUT_MS
 * @brief Per-call timeout for blocking HAL_UART_Transmit to the modem.
 *
 * The largest JSPR frame the library sends is ~2 KB
 * (PUT messageOriginateSegment with a 1447-byte base64 payload). At 230400
 * baud that's ~87 ms of wire time; 200 ms gives ~2x headroom and fails fast
 * on genuine UART faults (rather than the 1 s a stuck wire used to take).
 */
#ifndef RB9704_TX_TIMEOUT_MS
#define RB9704_TX_TIMEOUT_MS 200U
#endif

bool openPortStm32(void);
bool closePortStm32(void);
int  readStm32(char * bytes, const uint16_t length);
int  writeStm32(const char * data, const uint16_t length);
int  peekStm32(void);

/**
 * @brief Configure the library to use the given HAL UART instance.
 *
 * The UART must already be initialised by the CubeMX-generated
 * MX_USARTx_UART_Init() at 230400 baud, 8N1, no flow control.
 *
 * @param huart Pointer to the HAL UART handle.
 * @param baud  Must be 230400 (RB9704_BAUD).
 * @return true on success.
 */
bool setContextStm32(UART_HandleTypeDef * huart, const uint32_t baud);

/**
 * @brief Forward an RX-complete interrupt to the library.
 *
 * The user must call this from HAL_UART_RxCpltCallback() (and from
 * HAL_UARTEx_RxEventCallback if also used). The library re-arms the
 * interrupt internally; no further action needed.
 *
 * @code
 * void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart) {
 *     rb9704UartRxCpltCallback(huart);
 * }
 * @endcode
 */
void rb9704UartRxCpltCallback(UART_HandleTypeDef * huart);

/**
 * @brief Forward a UART error event to the library so it can recover.
 *
 * Optional but recommended. Call from HAL_UART_ErrorCallback() to clear
 * overrun/framing errors and re-arm the receive interrupt.
 *
 * @code
 * void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart) {
 *     rb9704UartErrorCallback(huart);
 * }
 * @endcode
 */
void rb9704UartErrorCallback(UART_HandleTypeDef * huart);

#ifdef __cplusplus
}
#endif

#endif /* SERIAL_STM32_H */
