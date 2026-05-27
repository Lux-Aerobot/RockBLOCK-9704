/*
 * RockBLOCK 9704 bring-up example for Nucleo-L452RE
 * (also targets STM32L452VCT6 in production -- same SRAM/peripheral layout).
 *
 * Behaviour:
 *   - Every 30 seconds, queue a "Hello from Lux" MO message.
 *   - On every MT message received, print it to the debug console.
 *   - Log MO/MT completions, provisioning, and signal changes to the console.
 *
 * Hardware wiring (Nucleo-L452RE):
 *   USART1 (RB9704 link, 230400 8N1):
 *       PA9  (D8 on Arduino header) -- TX  -> RB9704 RX (pin 14)
 *       PA10 (D2 on Arduino header) -- RX  <- RB9704 TX (pin 13)
 *       GND -- GND, 5V/3V3 to RB9704 VIN per its spec.
 *   USART2 (debug console, 115200 8N1):
 *       PA2 / PA3 -- already wired to the onboard ST-LINK VCP. Open a
 *       terminal on the ST-LINK virtual COM port at 115200 baud and you'll
 *       see output over the same USB cable that programs the board.
 *
 * Required CubeMX configuration:
 *   - USART1: 230400 baud, 8N1, no flow control, global interrupt ENABLED.
 *   - USART2: 115200 baud, 8N1, no flow control (already wizard-enabled
 *             when you pick "Board" -> Nucleo-L452RE in CubeMX).
 *   - Project-wide symbols (Project -> Properties -> C/C++ Build ->
 *     Settings -> MCU GCC Compiler -> Preprocessor):
 *         STM32_HAL
 *         IMT_PAYLOAD_SIZE=4096U
 *         IMT_QUEUE_SIZE=2U
 *     (160 KB SRAM on the L452 leaves plenty of room for these.)
 *
 * How to use this file:
 *   This mirrors a CubeMX-generated main.c. If you already have a CubeMX
 *   project, copy the marked sections (USER CODE BEGIN ...) into your own
 *   main.c rather than overwriting it -- CubeMX regenerates main.c every
 *   time you change the .ioc.
 */

#include "main.h"           /* CubeMX-generated; pulls in stm32l4xx_hal.h */
#include "rockblock_9704.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* CubeMX generates these in main.c. They are extern here so this file can be
 * compiled standalone for documentation; in your real main.c they live in
 * the same translation unit. */
extern UART_HandleTypeDef huart1;       /* RB9704 link */
extern UART_HandleTypeDef huart2;       /* console     */

/* ------------------------------------------------------------------------- */
/* Console output (UART2 -> ST-LINK VCP)                                     */
/* ------------------------------------------------------------------------- */

/* USER CODE BEGIN PFP */
static void consolePrintf(const char * fmt, ...)
{
    char buf[160];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof(buf)) n = sizeof(buf);
    HAL_UART_Transmit(&huart2, (uint8_t *)buf, (uint16_t)n, 100);
}
/* USER CODE END PFP */

/* ------------------------------------------------------------------------- */
/* RockBLOCK callbacks                                                       */
/* ------------------------------------------------------------------------- */

/* USER CODE BEGIN 0 */
static volatile bool receivedNewMessage = false;
static int8_t        currentSignal      = -1;

static void onMessageProvisioning(const jsprMessageProvisioning_t * info)
{
    if (info->provisioningSet)
    {
        consolePrintf("Provisioned for %d topics:\r\n", info->topicCount);
        for (int i = 0; i < info->topicCount; i++)
        {
            consolePrintf("  %s (%d)\r\n",
                          info->provisioning[i].topicName,
                          info->provisioning[i].topicId);
        }
    }
}

static void onMoComplete(const uint16_t id, const rbMsgStatus_t status)
{
    consolePrintf("MO complete id=%u status=%d\r\n", id, (int)status);
}

static void onMtComplete(const uint16_t id, const rbMsgStatus_t status)
{
    consolePrintf("MT complete id=%u status=%d\r\n", id, (int)status);
    if (status == RB_MSG_STATUS_OK)
    {
        receivedNewMessage = true;
    }
}

static void onConstellationState(const jsprConstellationState_t * state)
{
    if (state->signalBars != currentSignal)
    {
        currentSignal = state->signalBars;
        consolePrintf("Signal bars: %d\r\n", (int)currentSignal);
    }
}

/* ------------------------------------------------------------------------- */
/* HAL UART callbacks -- forward to the library                              */
/* ------------------------------------------------------------------------- */

void HAL_UART_RxCpltCallback(UART_HandleTypeDef * huart)
{
    if (huart == &huart1)
    {
        rb9704UartRxCpltCallback(huart);
    }
}

void HAL_UART_ErrorCallback(UART_HandleTypeDef * huart)
{
    if (huart == &huart1)
    {
        rb9704UartErrorCallback(huart);
    }
}
/* USER CODE END 0 */

/* ------------------------------------------------------------------------- */
/* main                                                                      */
/* ------------------------------------------------------------------------- */

int main(void)
{
    /* These calls come from CubeMX-generated main.c -- keep yours as-is. */
    HAL_Init();
    SystemClock_Config();
    MX_GPIO_Init();
    MX_USART1_UART_Init();
    MX_USART2_UART_Init();

    /* USER CODE BEGIN 2 */
    consolePrintf("\r\n=== RB9704 bring-up on Nucleo-L452RE ===\r\n");

    const rbCallbacks_t cbs = {
        .messageProvisioning = onMessageProvisioning,
        .moMessageComplete   = onMoComplete,
        .mtMessageComplete   = onMtComplete,
        .constellationState  = onConstellationState,
    };
    rbRegisterCallbacks(&cbs);

    if (!rbBegin(&huart1))
    {
        consolePrintf("rbBegin failed -- check wiring, baud, RB power\r\n");
        while (1) { HAL_Delay(1000); }
    }
    consolePrintf("rbBegin OK\r\n");
    HAL_Delay(100);   /* See README FAQ: avoid 418 on first send after reboot */

    const char     helloMsg[]  = "Hello from Lux";
    const size_t   helloLen    = sizeof(helloMsg) - 1U;
    const uint32_t sendIntMs   = 30000U;
    uint32_t       lastSendMs  = HAL_GetTick() - sendIntMs;  /* send immediately */

    /* Sized to comfortably fit a multi-segment MT on the L452's 160 KB SRAM.
     * Bump up to IMT_PAYLOAD_SIZE if you expect maximum-size MTs. */
    static char    mtCopy[2048];
    char *         mtBuffer    = NULL;
    /* USER CODE END 2 */

    /* USER CODE BEGIN WHILE */
    while (1)
    {
        rbPoll();

        /* --- periodic MO ---------------------------------------------- */
        if ((HAL_GetTick() - lastSendMs) >= sendIntMs)
        {
            if (rbSendMessageAsync(RAW_TOPIC, helloMsg, helloLen))
            {
                consolePrintf("Queued MO: \"%s\"\r\n", helloMsg);
                lastSendMs = HAL_GetTick();
            }
            else
            {
                /* MO queue full -- previous send still pending. Try again next
                 * iteration; don't update lastSendMs so we retry promptly. */
            }
        }

        /* --- handle MT ------------------------------------------------ */
        if (receivedNewMessage)
        {
            receivedNewMessage = false;
            const size_t mtLen = rbReceiveMessageAsync(&mtBuffer);
            if (mtLen > 0U && mtBuffer != NULL)
            {
                size_t copyLen = (mtLen < sizeof(mtCopy)) ? mtLen : sizeof(mtCopy);
                memcpy(mtCopy, mtBuffer, copyLen);

                /* Sanitize in-place so we can transmit the body in one shot.
                 * Non-printables become '.'. We do NOT NUL-terminate -- we
                 * pass an explicit length to HAL_UART_Transmit below. */
                for (size_t i = 0; i < copyLen; i++)
                {
                    if (mtCopy[i] < 32 || mtCopy[i] > 126)
                    {
                        mtCopy[i] = '.';
                    }
                }

                /* Three transmits, regardless of MT size:
                 *   1) prefix line, 2) sanitized body, 3) CRLF.
                 * Body timeout sized for full 2 KB buffer at 115200 baud
                 * (~180 ms) with headroom. */
                consolePrintf("MT (%u bytes): ", (unsigned)mtLen);
                HAL_UART_Transmit(&huart2, (uint8_t *)mtCopy, (uint16_t)copyLen, 500);
                HAL_UART_Transmit(&huart2, (uint8_t *)"\r\n", 2, 50);

                rbAcknowledgeReceiveHeadAsync();   /* free the slot */
            }
        }

        /* rbPoll() must run every <= 50 ms. 5 ms gives ample headroom. */
        HAL_Delay(5);
    }
    /* USER CODE END WHILE */
}
