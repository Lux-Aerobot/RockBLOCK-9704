/*
 * RB9704 modem-manager firmware — POC, Step 1: power sequencing + interlock
 * Target: Nucleo-L452RE (dev stand-in for the eventual dedicated modem MCU)
 *
 * THIS STEP (1 of 5):
 *   Implement and validate the 9704 startup / shutdown GPIO sequencing,
 *   triggered by the user button. Goal: confirm interlock correctness and
 *   read the real edge timing off the console, BEFORE any modem traffic.
 *   USART1 (to the 9704) is configured but NOT used to talk yet — its TX
 *   pin is held safely low until boot completes.
 *
 * No modem is required for this step: with SIMULATE_IBTD defined, the MCU
 * drives a spare GPIO (I_BTD_SIM) that you JUMPER to the I_BTD input, so the
 * interlock exercises the *real* GPIO read path and real electrical edges —
 * the MCU just plays the modem's I_BTD response with configurable delays.
 * Flip SIMULATE_IBTD off (and remove the jumper) once the real 9704 is wired.
 *
 * ---------------------------------------------------------------------------
 * RB9704 startup sequence (16-pin connector), per Ground Control docs:
 *   1. Set all pins connected to 9704 INPUTS tristate/low (GPS_EN exempt).
 *   2. Apply power to the 9704.
 *   3. Tristate / logic-high / pull-up I_EN.
 *   4. Wait for I_BTD to go HIGH.
 *   5. UART / input pins can now be initialised.
 * Shutdown sequence:
 *   1. Cease serial communications.
 *   2. Drive I_EN LOW.
 *   3. Wait for I_BTD to go LOW.
 *   4. Set all 9704-input pins tristate/low.
 *   5. Power may be removed.
 * DAMAGE INTERLOCK: once I_EN is driven HIGH, wait for I_BTD HIGH before
 * driving I_EN LOW again; once driven LOW, wait for I_BTD LOW before driving
 * HIGH again. Failure may damage the module. The state machine enforces this
 * structurally — a button press is only honoured from a stable state
 * (IDLE / RUNNING / FAULT), never mid-sequence.
 * "When not booted, the 9704 output states (except I_BTD) are undefined."
 *
 * ---------------------------------------------------------------------------
 * Wiring (9704 16-pin  ->  Nucleo-L452RE). Nucleo pins are easily reassigned
 * in CubeMX; the function mapping is what matters.
 *
 *   9704 pin / label                 dir(rel. 9704)   Nucleo
 *   3  I_EN   Iridium Enable          IN  <- host      PC1  (output PP)
 *   6  P_EN   Cap-charge enable (ACT.LOW) IN <- host   tie to GND -- NOT a
 *                                                       firmware signal (don't
 *                                                       confuse with PWR_EN)
 *   7  I_BTD  Booted signal           OUT -> host      PC2  (input, pull-down)
 *   13 TXD    9704 UART TX            OUT -> host      PA10 (USART1_RX, step 2+)
 *   14 RXD    9704 UART RX            IN  <- host      PA9  (USART1_TX / GPIO-low)
 *   1/4/10/16 GND                                       GND
 *   15 V_IN+  DC power 4.0-5.3V       IN               from our PWR_EN gate
 *
 *   Our power gate: PWR_EN  PC0 -> load-switch EN that applies/removes V_IN+
 *                   to the modem. This is the firmware "power" control.
 *   SIMULATE only:  I_BTD_SIM  PC3 (output PP) --- jumper to --- I_BTD PC2
 *
 *   On-board: B1 user button PC13, LD2 LED PA5, USART2 (PA2/PA3) -> ST-LINK VCP.
 *
 * Modem power: our PWR_EN signal (PC0) drives a load-switch gate that applies
 * or removes the modem's main power (V_IN+) entirely. It is NOT the 9704's
 * pin 6. (9704 pin 6 P_EN is the modem's *internal* cap-charge enable — tie it
 * to GND so the charger runs whenever the modem is powered; no firmware role.)
 * Gate polarity depends on the chosen load switch — set PWR_GATE_ACTIVE_HIGH
 * to match. Because we apply power only after the modem's input pins are
 * safe-low, and remove power only after they are returned low, there is no
 * back-power path through the modem's logic inputs.
 *
 * I_EN drive: the docs allow direct MCU drive or open-drain. We use push-pull
 * (HIGH = enable). Because we always drive I_EN LOW before removing power, no
 * back-power path exists. Open-drain is a valid alternative if preferred.
 *
 * Logic levels: 9704 logic-out HIGH is 2.9-3.4 V, logic-in HIGH min 2.0 V —
 * directly compatible with the L452 at 3.3 V, no level shifting needed.
 *
 * ---------------------------------------------------------------------------
 * CubeMX setup for this file:
 *   - USART2: 115200 8N1 (console via ST-LINK VCP). Keep CubeMX's default.
 *   - USART1: 230400 8N1 — but do NOT assign PA9 to USART1_TX in the .ioc.
 *     Leave PA9 as a GPIO output (we switch it to AF7 in firmware at boot).
 *     PA10 may stay USART1_RX (only used from step 2).
 *   - PC0/PC1/PC2/PC3, PA9, button, LED: this file's modem_pins_init()
 *     configures them in USER CODE, so they survive CubeMX regeneration.
 *   - Define SIMULATE_IBTD (e.g. project symbol) for bench testing.
 */

#include "main.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>

/* ====== build-time config ================================================ */
#define SIMULATE_IBTD                 /* comment out when the real 9704 is wired */

#define PWR_SETTLE_MS            100U  /* wait after applying power before I_EN high */
#define IBTD_BOOT_TIMEOUT_MS   30000U  /* generous; tighten once real boot time known */
#define IBTD_SHUTDOWN_TIMEOUT_MS 30000U
#define BTN_DEBOUNCE_MS           30U

#ifdef SIMULATE_IBTD
#define IBTD_SIM_BOOT_DELAY_MS     2000U  /* modelled modem boot time */
#define IBTD_SIM_SHUTDOWN_DELAY_MS 1000U  /* modelled modem shutdown time */
#endif

/* PWR_GATE_ACTIVE_HIGH: 1 if the load-switch enable is active-high (drive HIGH
 * to apply modem power), 0 if active-low. Set to match the chosen switch. */
#define PWR_GATE_ACTIVE_HIGH      1

/* ====== pin map (Nucleo-L452RE; reassign freely) ========================= */
#define PWR_PORT       GPIOC
#define PWR_PIN        GPIO_PIN_0      /* PWR_EN -> load-switch (applies/removes V_IN+) */
#define IEN_PORT       GPIOC
#define IEN_PIN        GPIO_PIN_1      /* 9704 pin 3  I_EN */
#define IBTD_PORT      GPIOC
#define IBTD_PIN       GPIO_PIN_2      /* 9704 pin 7  I_BTD (input) */
#define IBTD_SIM_PORT  GPIOC
#define IBTD_SIM_PIN   GPIO_PIN_3      /* SIM: jumper to IBTD_PIN */
#define U1TX_PORT      GPIOA
#define U1TX_PIN       GPIO_PIN_9      /* 9704 pin 14 RXD = our USART1 TX */
#define BTN_PORT       GPIOC
#define BTN_PIN        GPIO_PIN_13     /* B1 (idle high, pressed low) */
#define LED_PORT       GPIOA
#define LED_PIN        GPIO_PIN_5      /* LD2 */

/* ====== level helpers ==================================================== */
#if PWR_GATE_ACTIVE_HIGH
#define PWR_ON()    HAL_GPIO_WritePin(PWR_PORT, PWR_PIN, GPIO_PIN_SET)    /* gate on  = power on  */
#define PWR_OFF()   HAL_GPIO_WritePin(PWR_PORT, PWR_PIN, GPIO_PIN_RESET)  /* gate off = power off */
#define PWR_IS_ON() (HAL_GPIO_ReadPin(PWR_PORT, PWR_PIN) == GPIO_PIN_SET)
#else
#define PWR_ON()    HAL_GPIO_WritePin(PWR_PORT, PWR_PIN, GPIO_PIN_RESET)
#define PWR_OFF()   HAL_GPIO_WritePin(PWR_PORT, PWR_PIN, GPIO_PIN_SET)
#define PWR_IS_ON() (HAL_GPIO_ReadPin(PWR_PORT, PWR_PIN) == GPIO_PIN_RESET)
#endif
#define IEN_HIGH()  HAL_GPIO_WritePin(IEN_PORT, IEN_PIN, GPIO_PIN_SET)
#define IEN_LOW()   HAL_GPIO_WritePin(IEN_PORT, IEN_PIN, GPIO_PIN_RESET)
#define IEN_IS_HIGH() (HAL_GPIO_ReadPin(IEN_PORT, IEN_PIN) == GPIO_PIN_SET)
#define IBTD_HIGH() (HAL_GPIO_ReadPin(IBTD_PORT, IBTD_PIN) == GPIO_PIN_SET)
#define LED_ON()    HAL_GPIO_WritePin(LED_PORT, LED_PIN, GPIO_PIN_SET)
#define LED_OFF()   HAL_GPIO_WritePin(LED_PORT, LED_PIN, GPIO_PIN_RESET)

/* CubeMX provides this (USART2 console). */
extern UART_HandleTypeDef huart2;

/* ====== state machine ==================================================== */
typedef enum {
    ST_IDLE,      /* power off, inputs safe-low, waiting for trigger */
    ST_STARTUP,   /* power applied, driving I_EN high, waiting I_BTD high */
    ST_RUNNING,   /* booted, UART live (step 2+) */
    ST_SHUTDOWN,  /* I_EN low, waiting I_BTD low */
    ST_FAULT      /* interlock timeout / error; power removed, awaiting ack */
} mstate_t;

static mstate_t  g_state = ST_IDLE;
static uint32_t  g_phase_ms = 0;       /* timestamp of the last intra-state action */
static bool      g_ien_committed = false; /* has I_EN been driven this sequence? */
static bool      g_last_ien_high = false; /* interlock guard: last commanded I_EN level */

static const char *state_name(mstate_t s)
{
    switch (s) {
        case ST_IDLE:     return "IDLE";
        case ST_STARTUP:  return "STARTUP";
        case ST_RUNNING:  return "RUNNING";
        case ST_SHUTDOWN: return "SHUTDOWN";
        case ST_FAULT:    return "FAULT";
        default:          return "?";
    }
}

/* ====== console ========================================================== */
static void consolePrintf(const char *fmt, ...)
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

/* ms timestamp prefix; ##__VA_ARGS__ keeps the tick as the first arg and
 * tolerates calls with no further args (GNU extension, supported by arm-gcc). */
#define LOG(fmt, ...) consolePrintf("[%8lu] " fmt, (unsigned long)HAL_GetTick(), ##__VA_ARGS__)

/* ====== USART1 TX pin: force-low vs alternate-function ==================== */
/* Step 1 validates "force the host TX (9704 RXD) low until booted". */
static void uart1_tx_force_low(void)
{
    GPIO_InitTypeDef g = {0};
    HAL_GPIO_WritePin(U1TX_PORT, U1TX_PIN, GPIO_PIN_RESET);
    g.Pin   = U1TX_PIN;
    g.Mode  = GPIO_MODE_OUTPUT_PP;
    g.Pull  = GPIO_NOPULL;
    g.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(U1TX_PORT, &g);
    HAL_GPIO_WritePin(U1TX_PORT, U1TX_PIN, GPIO_PIN_RESET);
}

static void uart1_tx_to_af(void)
{
    GPIO_InitTypeDef g = {0};
    g.Pin       = U1TX_PIN;
    g.Mode      = GPIO_MODE_AF_PP;
    g.Pull      = GPIO_NOPULL;
    g.Speed     = GPIO_SPEED_FREQ_HIGH;
    g.Alternate = GPIO_AF7_USART1;
    HAL_GPIO_Init(U1TX_PORT, &g);
}

/* ====== GPIO init (USER CODE so CubeMX regen won't clobber it) =========== */
static void modem_pins_init(void)
{
    GPIO_InitTypeDef g = {0};
    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_GPIOC_CLK_ENABLE();

    /* Safe levels BEFORE configuring as outputs, to avoid glitches. */
    PWR_OFF();   /* power gate off = modem unpowered */
    IEN_LOW();   /* I_EN low  = disabled  */

    /* PWR_EN, I_EN: push-pull outputs */
    g.Pin   = PWR_PIN;
    g.Mode  = GPIO_MODE_OUTPUT_PP;
    g.Pull  = GPIO_NOPULL;
    g.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(PWR_PORT, &g);
    g.Pin = IEN_PIN;
    HAL_GPIO_Init(IEN_PORT, &g);

    /* I_BTD: input, pull-down (read clean low when modem not driving) */
    g.Pin  = IBTD_PIN;
    g.Mode = GPIO_MODE_INPUT;
    g.Pull = GPIO_PULLDOWN;
    HAL_GPIO_Init(IBTD_PORT, &g);

#ifdef SIMULATE_IBTD
    /* I_BTD_SIM: push-pull output, starts low (modem not booted) */
    HAL_GPIO_WritePin(IBTD_SIM_PORT, IBTD_SIM_PIN, GPIO_PIN_RESET);
    g.Pin   = IBTD_SIM_PIN;
    g.Mode  = GPIO_MODE_OUTPUT_PP;
    g.Pull  = GPIO_NOPULL;
    g.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(IBTD_SIM_PORT, &g);
#endif

    /* LED */
    LED_OFF();
    g.Pin   = LED_PIN;
    g.Mode  = GPIO_MODE_OUTPUT_PP;
    g.Pull  = GPIO_NOPULL;
    g.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(LED_PORT, &g);

    /* Button: input, pull-up (idle high, pressed low). Adjust if your board
     * variant differs. */
    g.Pin  = BTN_PIN;
    g.Mode = GPIO_MODE_INPUT;
    g.Pull = GPIO_PULLUP;
    HAL_GPIO_Init(BTN_PORT, &g);

    /* Host TX (9704 RXD): forced low until boot completes (startup step 1). */
    uart1_tx_force_low();
}

/* ====== debounced button (returns true once per press) =================== */
static bool button_pressed_edge(void)
{
    static GPIO_PinState lastRaw = GPIO_PIN_SET;
    static GPIO_PinState stable  = GPIO_PIN_SET;
    static uint32_t      changed = 0;
    GPIO_PinState raw = HAL_GPIO_ReadPin(BTN_PORT, BTN_PIN);
    uint32_t now = HAL_GetTick();

    if (raw != lastRaw) { lastRaw = raw; changed = now; }
    if ((now - changed) >= BTN_DEBOUNCE_MS && stable != raw) {
        stable = raw;
        if (stable == GPIO_PIN_RESET) return true;   /* falling edge = press */
    }
    return false;
}

#ifdef SIMULATE_IBTD
/* Model the modem: I_BTD follows (powered AND I_EN high), with delays. The
 * output drives I_BTD_SIM, which is jumpered to the real I_BTD input pin. */
static void sim_ibtd_update(void)
{
    static uint32_t boot_since = 0;
    static uint32_t down_since = 0;
    uint32_t now = HAL_GetTick();
    bool want_boot = PWR_IS_ON() && IEN_IS_HIGH();

    if (want_boot) {
        down_since = 0;
        if (boot_since == 0) boot_since = now;
        if ((now - boot_since) >= IBTD_SIM_BOOT_DELAY_MS) {
            HAL_GPIO_WritePin(IBTD_SIM_PORT, IBTD_SIM_PIN, GPIO_PIN_SET);
        }
    } else {
        boot_since = 0;
        if (down_since == 0) down_since = now;
        if ((now - down_since) >= IBTD_SIM_SHUTDOWN_DELAY_MS) {
            HAL_GPIO_WritePin(IBTD_SIM_PORT, IBTD_SIM_PIN, GPIO_PIN_RESET);
        }
    }
}
#endif

static void enter_state(mstate_t s)
{
    g_state = s;
    g_phase_ms = HAL_GetTick();
    g_ien_committed = false;
    LOG("--> %s\r\n", state_name(s));
}

static void enter_fault(const char *why)
{
    LOG("FAULT: %s\r\n", why);
    /* Safe escape: remove power. The 9704 shuts down on power loss regardless
     * of I_EN/I_BTD, so this is valid even when the interlock is otherwise
     * blocking an I_EN change (e.g. boot timed out with I_BTD stuck low). */
    PWR_OFF();
    uart1_tx_force_low();
    enter_state(ST_FAULT);
}

static void sm_step(bool pressed)
{
    uint32_t now = HAL_GetTick();

    switch (g_state) {

    case ST_IDLE:
        /* Startup step 1 already satisfied here: power off, I_EN low, TX low. */
        if (pressed) {
            LOG("button: startup\r\n");
            PWR_ON();                       /* step 2: apply power (gate on) */
            LOG("power gate ON (power applied)\r\n");
            enter_state(ST_STARTUP);
        }
        break;

    case ST_STARTUP:
        if (!g_ien_committed) {
            if ((now - g_phase_ms) >= PWR_SETTLE_MS) {
                /* Interlock: only drive I_EN high from a known-low I_BTD. */
                if (IBTD_HIGH()) {
                    enter_fault("I_BTD already HIGH before boot requested");
                    break;
                }
                IEN_HIGH();                 /* step 3: enable */
                g_last_ien_high = true;
                g_ien_committed = true;
                g_phase_ms = now;
                LOG("I_EN high (boot requested)\r\n");
            }
        } else if (IBTD_HIGH()) {           /* step 4: booted */
            LOG("I_BTD HIGH after %lu ms\r\n", (unsigned long)(now - g_phase_ms));
            uart1_tx_to_af();               /* step 5: UART pins live (TX -> AF) */
            LOG("USART1 TX -> AF (UART would init here in step 2+)\r\n");
            enter_state(ST_RUNNING);
        } else if ((now - g_phase_ms) >= IBTD_BOOT_TIMEOUT_MS) {
            enter_fault("I_BTD never went HIGH");
        }
        break;

    case ST_RUNNING:
        if (pressed) {
            LOG("button: shutdown\r\n");
            uart1_tx_force_low();           /* step 1: cease comms / TX low */
            IEN_LOW();                      /* step 2: I_EN low */
            g_last_ien_high = false;
            LOG("TX low, I_EN low (shutdown requested)\r\n");
            /* enter_state() sets g_phase_ms = now = the moment I_EN went low;
             * ST_SHUTDOWN times its I_BTD-low wait from that. */
            enter_state(ST_SHUTDOWN);
        }
        break;

    case ST_SHUTDOWN:
        if (!IBTD_HIGH()) {                 /* step 3: confirmed shut down */
            LOG("I_BTD LOW after %lu ms\r\n", (unsigned long)(now - g_phase_ms));
            /* step 4 (inputs already safe-low) + step 5: remove power */
            PWR_OFF();
            LOG("power gate OFF (power removed)\r\n");
            enter_state(ST_IDLE);
        } else if ((now - g_phase_ms) >= IBTD_SHUTDOWN_TIMEOUT_MS) {
            enter_fault("I_BTD never went LOW");
        }
        break;

    case ST_FAULT:
        /* Power already removed in enter_fault(). Once I_BTD has dropped
         * (modem off), it is safe to drive I_EN low. Operator acknowledges
         * with a button press to return to IDLE. (Future: Core RESET command
         * funnels into this same recovery.) */
        if (!IBTD_HIGH()) {
            IEN_LOW();
            g_last_ien_high = false;
        }
        if (pressed && !IBTD_HIGH()) {
            LOG("button: FAULT acknowledged\r\n");
            enter_state(ST_IDLE);
        }
        break;
    }
}

/* ====== LED: at-a-glance state =========================================== */
static void update_led(void)
{
    uint32_t t = HAL_GetTick();
    switch (g_state) {
        case ST_IDLE:     LED_OFF(); break;
        case ST_RUNNING:  LED_ON();  break;
        case ST_STARTUP:  (t % 200U  < 100U) ? LED_ON() : LED_OFF(); break; /* fast blink */
        case ST_SHUTDOWN: (t % 800U  < 400U) ? LED_ON() : LED_OFF(); break; /* slow blink */
        case ST_FAULT:    (t % 120U  <  60U) ? LED_ON() : LED_OFF(); break; /* frantic */
    }
}

int main(void)
{
    /* CubeMX-generated bring-up. */
    HAL_Init();
    SystemClock_Config();
    MX_GPIO_Init();            /* CubeMX may configure other pins; ours below */
    MX_USART2_UART_Init();     /* console */
    /* USART1 intentionally NOT initialised in step 1. */

    /* USER CODE BEGIN 2 */
    modem_pins_init();         /* safe levels: power off, I_EN low, TX low */
    enter_state(ST_IDLE);

    consolePrintf("\r\n=== RB9704 modem-manager — step 1: power sequencing ===\r\n");
#ifdef SIMULATE_IBTD
    consolePrintf("SIMULATE_IBTD: ON  (jumper PC3 -> PC2 so the sim feeds I_BTD)\r\n");
    consolePrintf("  modelled boot=%lums shutdown=%lums\r\n",
                  (unsigned long)IBTD_SIM_BOOT_DELAY_MS,
                  (unsigned long)IBTD_SIM_SHUTDOWN_DELAY_MS);
#else
    consolePrintf("SIMULATE_IBTD: OFF (real 9704 drives I_BTD)\r\n");
#endif
    consolePrintf("Press the user button (B1) to start up / shut down.\r\n\r\n");

    while (1) {
        bool pressed = button_pressed_edge();
#ifdef SIMULATE_IBTD
        sim_ibtd_update();
#endif
        sm_step(pressed);
        update_led();
        HAL_Delay(2);
    }
    /* USER CODE END 2 */
}
