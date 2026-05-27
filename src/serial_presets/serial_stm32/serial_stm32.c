#if defined(STM32_HAL)

#include "serial_stm32.h"
#include "../../serial.h"

extern enum serialState serialState;
extern serialContext context;

static UART_HandleTypeDef * rb9704Huart  = NULL;
static volatile uint8_t     rxByte;
static volatile uint8_t     rxRing[RB9704_RX_RING_SIZE];
static volatile uint16_t    rxHead = 0U;
static volatile uint16_t    rxTail = 0U;

#define RING_MASK ((uint16_t)(RB9704_RX_RING_SIZE - 1U))

static inline uint16_t ringCount(void)
{
    return (uint16_t)((rxHead - rxTail) & RING_MASK);
}

bool openPortStm32(void)
{
    if (rb9704Huart == NULL)
    {
        return false;
    }

    rxHead = 0U;
    rxTail = 0U;

    if (HAL_UART_Receive_IT(rb9704Huart, (uint8_t *)&rxByte, 1U) != HAL_OK)
    {
        return false;
    }

    serialState = OPEN;
    return true;
}

bool closePortStm32(void)
{
    if (rb9704Huart != NULL)
    {
        HAL_UART_AbortReceive_IT(rb9704Huart);
    }
    serialState = CLOSED;
    return true;
}

int readStm32(char * bytes, const uint16_t length)
{
    uint16_t n = 0U;
    while (n < length)
    {
        if (rxHead == rxTail)
        {
            break;
        }
        bytes[n++] = (char)rxRing[rxTail];
        rxTail = (uint16_t)((rxTail + 1U) & RING_MASK);
    }
    return (int)n;
}

int writeStm32(const char * data, const uint16_t length)
{
    if (rb9704Huart == NULL)
    {
        return 0;
    }
    if (HAL_UART_Transmit(rb9704Huart, (uint8_t *)data, length, RB9704_TX_TIMEOUT_MS) == HAL_OK)
    {
        return (int)length;
    }
    return 0;
}

int peekStm32(void)
{
    return (int)ringCount();
}

bool setContextStm32(UART_HandleTypeDef * huart, const uint32_t baud)
{
    bool ok = false;

    if (huart == NULL)
    {
        return false;
    }

    rb9704Huart          = huart;
    context.serialBaud   = baud;
    context.serialInit   = openPortStm32;
    context.serialDeInit = closePortStm32;
    context.serialRead   = readStm32;
    context.serialWrite  = writeStm32;
    context.serialPeek   = peekStm32;

    /* Mirror the Arduino preset: open then close to verify the handle works. */
    if (context.serialInit())
    {
        if (context.serialDeInit())
        {
            ok = true;
        }
    }
    return ok;
}

void rb9704UartRxCpltCallback(UART_HandleTypeDef * huart)
{
    if (huart != rb9704Huart)
    {
        return;
    }

    uint16_t next = (uint16_t)((rxHead + 1U) & RING_MASK);
    if (next != rxTail)
    {
        rxRing[rxHead] = rxByte;
        rxHead = next;
    }
    /* If next == rxTail we drop this byte (overrun). The library reads from
     * the ring buffer in rbPoll() — call it more frequently if you see drops. */

    (void)HAL_UART_Receive_IT(rb9704Huart, (uint8_t *)&rxByte, 1U);
}

void rb9704UartErrorCallback(UART_HandleTypeDef * huart)
{
    if (huart != rb9704Huart)
    {
        return;
    }

    /* HAL has already cleared error flags by the time this callback fires
     * (since F4 HAL v1.6 / similar across families). Just re-arm the IT
     * receive in case the error stopped it. */
    if (HAL_UART_GetState(rb9704Huart) != HAL_UART_STATE_BUSY_RX)
    {
        (void)HAL_UART_Receive_IT(rb9704Huart, (uint8_t *)&rxByte, 1U);
    }
}

#endif /* STM32_HAL */
