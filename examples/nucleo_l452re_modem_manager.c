/*
 * RB9704 modem-manager firmware — POC, Step 1: power sequencing + interlock
 * Target: Nucleo-L452RE (dev stand-in for the eventual dedicated modem MCU)
 *
 * STEPS:
 *   1 (done, hardware-validated): button-triggered GPIO startup/shutdown
 *     sequencing + damage interlock + FAULT recovery, with SIMULATE_IBTD.
 *   2.1 (this change): bring USART1 UP (HAL init) at boot and DOWN (deinit)
 *     at shutdown, at the correct points in the sequence. PA9 now transitions
 *     GPIO-low (pre-boot) -> real UART idle-high (RUNNING) -> GPIO-low
 *     (shutdown). Still no UART traffic — that is 2.2+.
 *
 * No modem is required to bench this: with SIMULATE_IBTD defined, the MCU
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
 * CubeMX setup for this file (step 2.1):
 *   - Board defaults: USART2 (115200 8N1) on the ST-LINK VCP, B1 button,
 *     LD2 LED, default clock.
 *   - NOW configure USART1: Connectivity -> USART1 -> Asynchronous,
 *     230400 8N1 (assigns PA9 TX / PA10 RX as AF7). Then DEFER its auto-init:
 *     Project Manager -> Advanced Settings -> in the generated-function-call
 *     list, UNCHECK the call to MX_USART1_UART_Init() (the function is still
 *     generated, just not called at startup). We call it ourselves at boot
 *     (uart1_up) and HAL_UART_DeInit at shutdown (uart1_down). That is what
 *     lets PA9 be held GPIO-low pre-boot and only handed to USART1 once
 *     I_BTD is high — no conflict, by design.
 *   - You do NOT need to add PC0/PC1/PC2/PC3 in CubeMX — modem_pins_init()
 *     configures them in USER CODE. PA9 is owned by USART1 in CubeMX now, but
 *     modem_pins_init() re-points it to GPIO-low until uart1_up() runs.
 *   - SIMULATE_IBTD is #defined at the top of this file (already ON). To
 *     switch to the real modem, comment it out (and remove the I_BTD jumper).
 */

#include "main.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>

/* ====== build-time config ================================================ */
#define SIMULATE_IBTD                 /* comment out when the real 9704 is wired */

/* Sequence settle margins — symmetric up/down:
 *   startup:  PWR_ON -[PWR_SETTLE_MS]- I_EN high -wait I_BTD high-
 *             -[UART_UP_DELAY_MS]- USART1 up -> RUNNING
 *   shutdown: USART1 down -[UART_DOWN_DELAY_MS]- I_EN low -wait I_BTD low-
 *             -[PWR_OFF_DELAY_MS]- power off -> IDLE
 */
#define PWR_SETTLE_MS            100U  /* power applied -> before I_EN high */
#define UART_UP_DELAY_MS         100U  /* I_BTD high    -> before USART1 up */
#define UART_DOWN_DELAY_MS       100U  /* USART1 down   -> before I_EN low  */
#define PWR_OFF_DELAY_MS         100U  /* I_BTD low     -> before power off */
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

/* CubeMX provides these. USART2 = console (auto-init at startup). USART1 =
 * modem link — its auto-init call is DISABLED in CubeMX Advanced Settings, so
 * we bring it up/down in user code at the sequence points (step 2.1). */
extern UART_HandleTypeDef huart1;
extern UART_HandleTypeDef huart2;
void MX_USART1_UART_Init(void);

/* ====== state machine ==================================================== */
typedef enum {
    ST_IDLE,      /* power off, inputs safe-low, waiting for trigger */
    ST_STARTUP,   /* power applied, driving I_EN high, waiting I_BTD high */
    ST_RUNNING,   /* booted, USART1 up (modem link live) */
    ST_SHUTDOWN,  /* I_EN low, waiting I_BTD low */
    ST_FAULT      /* interlock timeout / error; power removed, awaiting ack */
} mstate_t;

static mstate_t  g_state = ST_IDLE;
static uint32_t  g_phase_ms = 0;       /* timestamp of the last intra-state action */
static bool      g_ien_committed = false; /* has the I_EN transition for this state been done? */
static bool      g_last_ien_high = false; /* interlock guard: last commanded I_EN level */
static bool      g_ibtd_high_seen = false;/* startup: I_BTD high confirmed, in UART-up settle */
static bool      g_ibtd_low_seen = false; /* shutdown: I_BTD low confirmed, in power-off settle */

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

/* Bring USART1 up (startup step 5, after I_BTD high). MX_USART1_UART_Init()'s
 * MspInit puts PA9/PA10 to AF7 and enables the peripheral, so PA9 now idles at
 * a real UART high — unlike step 1, where the pin was parked in AF with the
 * peripheral disabled (no defined idle level). */
static void uart1_up(void)
{
    MX_USART1_UART_Init();
}

/* Bring USART1 down (shutdown step 1, "cease serial communications"). DeInit
 * releases PA9/PA10 (MspDeInit), then we re-assert the host TX low for the
 * unpowered phase. */
static void uart1_down(void)
{
    HAL_UART_DeInit(&huart1);
    uart1_tx_force_low();
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
    g_ibtd_high_seen = false;
    g_ibtd_low_seen = false;
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
        } else if (!g_ibtd_high_seen) {     /* step 4: wait for booted */
            if (IBTD_HIGH()) {
                LOG("I_BTD HIGH after %lu ms\r\n", (unsigned long)(now - g_phase_ms));
                g_ibtd_high_seen = true;
                g_phase_ms = now;           /* repurpose: I_BTD-high timestamp for the UART-up settle */
            } else if ((now - g_phase_ms) >= IBTD_BOOT_TIMEOUT_MS) {
                enter_fault("I_BTD never went HIGH");
            }
        } else if ((now - g_phase_ms) >= UART_UP_DELAY_MS) {
            uart1_up();                     /* step 5: USART1 up (PA9 -> AF, peripheral enabled) */
            LOG("USART1 up -- TX now at real UART idle-high (%lu ms after I_BTD high)\r\n",
                (unsigned long)UART_UP_DELAY_MS);
            enter_state(ST_RUNNING);
        }
        break;

    case ST_RUNNING:
        if (pressed) {
            LOG("button: shutdown\r\n");
            uart1_down();                   /* step 1: cease comms (USART1 deinit + TX low) */
            LOG("USART1 down, TX low\r\n");
            /* enter_state() sets g_phase_ms = now = the USART1-down time;
             * ST_SHUTDOWN settles UART_DOWN_DELAY_MS before driving I_EN low. */
            enter_state(ST_SHUTDOWN);
        }
        break;

    case ST_SHUTDOWN:
        if (!g_ien_committed) {             /* settle after UART down, then step 2: I_EN low */
            if ((now - g_phase_ms) >= UART_DOWN_DELAY_MS) {
                IEN_LOW();
                g_last_ien_high = false;
                g_ien_committed = true;
                g_phase_ms = now;
                LOG("I_EN low (%lu ms after USART1 down)\r\n",
                    (unsigned long)UART_DOWN_DELAY_MS);
            }
        } else if (!g_ibtd_low_seen) {      /* step 3: wait for confirmed shut down */
            if (!IBTD_HIGH()) {
                LOG("I_BTD LOW after %lu ms\r\n", (unsigned long)(now - g_phase_ms));
                g_ibtd_low_seen = true;
                g_phase_ms = now;           /* repurpose: I_BTD-low timestamp for the power-off settle */
            } else if ((now - g_phase_ms) >= IBTD_SHUTDOWN_TIMEOUT_MS) {
                enter_fault("I_BTD never went LOW");
            }
        } else if ((now - g_phase_ms) >= PWR_OFF_DELAY_MS) {
            /* settle after I_BTD low so we don't cut power mid-housekeeping,
             * then step 4/5: remove power. */
            PWR_OFF();
            LOG("power gate OFF (power removed, %lu ms after I_BTD low)\r\n",
                (unsigned long)PWR_OFF_DELAY_MS);
            enter_state(ST_IDLE);
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
