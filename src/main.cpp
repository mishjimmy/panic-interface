/*
 * Contact-closure -> serial status sender
 * Target: LILYGO T-RSS3 (ESP32-S3, isolated RS232 + RS485 modules)
 *
 * A dry contact is wired between CONTACT_PIN and GND (internal pull-up).
 *   Closed = LOW  = normal
 *   Open   = HIGH = emergency   (a cut wire therefore fails safe into emergency)
 *
 * See README.md for wiring and the state diagram.
 */

#include <Arduino.h>

// ============================================================================
// =============================  CONFIGURATION  ==============================
// ============================================================================

// ---- Timings (seconds) -------------------------------------------------------
#define NORMAL_INTERVAL_S          60   // NORMAL: send MSG_NORMAL every 60 s
#define EMERG_FAST_INTERVAL_S       1   // EMERGENCY_FAST: send every 1 s ...
#define EMERG_FAST_DURATION_S      60   // ... for 1 min, then EMERGENCY_SLOW
#define EMERG_SLOW_INTERVAL_S      60   // EMERGENCY_SLOW: send every 60 s
#define RECOVERY_INTERVAL_S         1   // RECOVERY: send MSG_NORMAL every 1 s ...
#define RECOVERY_DURATION_S        60   // ... for 1 min, then NORMAL
#define CLEAR_CONFIRM_S             1   // contact must stay closed this long to leave emergency

// ---- Timings (milliseconds) - too short to express in whole seconds -----------
#define OPEN_DEBOUNCE_MS           50UL // contact must be open this long to trigger emergency

// ---- Contact input -----------------------------------------------------------
// GPIO6: J4 header pin 5 (adjacent to GND on J4 pins 6/7). Not a strapping pin,
// not used by the RS232/RS485 modules, LED, buttons, USB or flash.
#define CONTACT_PIN                  6

// ---- Messages ----------------------------------------------------------------
#define MSG_NORMAL       "not_in_emergency"
#define MSG_EMERGENCY    "emergency_active"
#define LINE_TERMINATOR  "\r\n"

// ---- Serial output -----------------------------------------------------------
#define BAUD_RATE        9600
#define SERIAL_CONFIG    SERIAL_8N1

// Output port selection: PORT_RS232, PORT_RS485 or PORT_BOTH
#define PORT_RS232       1
#define PORT_RS485       2
#define PORT_BOTH        (PORT_RS232 | PORT_RS485)
#define OUTPUT_PORT      PORT_RS232

// ---- Status LED (onboard WS2812) ---------------------------------------------
#define ENABLE_STATUS_LED    1      // 0 = disable LED entirely (library code not used)
#define LED_BRIGHTNESS       40     // 0-255 global brightness cap

// ---- USB debug console -------------------------------------------------------
#define DEBUG_BAUD           115200 // ignored by native USB CDC, kept for clarity

// ============================================================================
// =========================  BOARD PINS (do not edit)  =======================
// Source: github.com/Xinyuan-LilyGO/T-RSC3 (repo for "T-RSC3 & RSS3"),
//   example/RS485_simple/utilities.h, CONFIG_IDF_TARGET_ESP32S3 branch,
//   cross-checked against schematic/T-RSS3.pdf.
// ============================================================================
#define RS232_TX_PIN     41
#define RS232_RX_PIN     42
#define RS485_TX_PIN      3
#define RS485_RX_PIN      2
#define RS485_CON_PIN     4     // RSM485MT5V "CON": HIGH = transmit, LOW = receive
#define RS485_TX_ENABLE   HIGH
#define RS485_RX_ENABLE   LOW
#define LED_PIN           1     // WS2812C, GRB
// (BOOT button = GPIO0, optional user button S3 = GPIO5 - both unused here)

// ============================================================================

// ---- Seconds -> milliseconds (used by the code below) --------------------------
#define S_TO_MS(s)  ((uint32_t)(s) * 1000UL)
#define NORMAL_INTERVAL_MS      S_TO_MS(NORMAL_INTERVAL_S)
#define EMERG_FAST_INTERVAL_MS  S_TO_MS(EMERG_FAST_INTERVAL_S)
#define EMERG_FAST_DURATION_MS  S_TO_MS(EMERG_FAST_DURATION_S)
#define EMERG_SLOW_INTERVAL_MS  S_TO_MS(EMERG_SLOW_INTERVAL_S)
#define RECOVERY_INTERVAL_MS    S_TO_MS(RECOVERY_INTERVAL_S)
#define RECOVERY_DURATION_MS    S_TO_MS(RECOVERY_DURATION_S)
#define CLEAR_CONFIRM_MS        S_TO_MS(CLEAR_CONFIRM_S)

#define USE_RS232 ((OUTPUT_PORT & PORT_RS232) != 0)
#define USE_RS485 ((OUTPUT_PORT & PORT_RS485) != 0)

static_assert(OUTPUT_PORT >= PORT_RS232 && OUTPUT_PORT <= PORT_BOTH, "OUTPUT_PORT must be PORT_RS232, PORT_RS485 or PORT_BOTH");
static_assert(NORMAL_INTERVAL_MS > 0 && EMERG_FAST_INTERVAL_MS > 0 &&
              EMERG_SLOW_INTERVAL_MS > 0 && RECOVERY_INTERVAL_MS > 0, "intervals must be > 0");
static_assert(CONTACT_PIN != RS232_TX_PIN && CONTACT_PIN != RS232_RX_PIN &&
              CONTACT_PIN != RS485_TX_PIN && CONTACT_PIN != RS485_RX_PIN &&
              CONTACT_PIN != RS485_CON_PIN && CONTACT_PIN != LED_PIN && CONTACT_PIN != 0,
              "CONTACT_PIN collides with a board pin");

// Use UART1/UART2 so UART0 (ROM/core console) can never leak onto the RS232 line.
static HardwareSerial &Rs232 = Serial1;
static HardwareSerial &Rs485 = Serial2;

#if ENABLE_STATUS_LED
#include <Adafruit_NeoPixel.h>
static Adafruit_NeoPixel pixel(1, LED_PIN, NEO_GRB + NEO_KHZ800);
#endif

// ---- State machine -----------------------------------------------------------
enum State : uint8_t { NORMAL, EMERGENCY_FAST, EMERGENCY_SLOW, RECOVERY };

static const char *const STATE_NAMES[] = { "NORMAL", "EMERGENCY_FAST", "EMERGENCY_SLOW", "RECOVERY" };

static State    state;
static uint32_t stateEnteredAt;   // millis() when current state was entered
static uint32_t lastSendAt;       // scheduled time of the last periodic send

// Contact tracking (raw level + time it last changed)
static bool     rawOpen;
static uint32_t rawChangedAt;

static uint32_t intervalFor(State s) {
  switch (s) {
    case NORMAL:         return NORMAL_INTERVAL_MS;
    case EMERGENCY_FAST: return EMERG_FAST_INTERVAL_MS;
    case EMERGENCY_SLOW: return EMERG_SLOW_INTERVAL_MS;
    case RECOVERY:       return RECOVERY_INTERVAL_MS;
  }
  return NORMAL_INTERVAL_MS;
}

static const char *messageFor(State s) {
  return (s == EMERGENCY_FAST || s == EMERGENCY_SLOW) ? MSG_EMERGENCY : MSG_NORMAL;
}

// ---- Output ------------------------------------------------------------------
static void sendMessage(const char *msg) {
  const uint32_t now = millis();

#if USE_RS232
  Rs232.print(msg);
  Rs232.print(LINE_TERMINATOR);
  Serial.printf("[t=%lu ms] TX(RS232): %s\n", (unsigned long)now, msg);
#endif

#if USE_RS485
  digitalWrite(RS485_CON_PIN, RS485_TX_ENABLE);
  Rs485.print(msg);
  Rs485.print(LINE_TERMINATOR);
  Rs485.flush();                                  // wait until the last stop bit is out
  digitalWrite(RS485_CON_PIN, RS485_RX_ENABLE);
  Serial.printf("[t=%lu ms] TX(RS485): %s\n", (unsigned long)now, msg);
#endif
}

static void enterState(State next, uint32_t now) {
  Serial.printf("[t=%lu ms] STATE %s -> %s\n", (unsigned long)now,
                STATE_NAMES[state], STATE_NAMES[next]);
  state          = next;
  stateEnteredAt = now;
  lastSendAt     = now;
  sendMessage(messageFor(state));                 // first message immediately
}

// ---- Status LED --------------------------------------------------------------
#if ENABLE_STATUS_LED
static void updateLed(uint32_t now) {
  static uint32_t lastUpdate = 0;
  static uint32_t lastColor  = 0xFFFFFFFF;
  if (now - lastUpdate < 20) return;              // ~50 Hz refresh
  lastUpdate = now;

  const uint32_t t = now - stateEnteredAt;        // patterns phase-locked to state entry
  uint32_t color = 0;

  switch (state) {
    case NORMAL: {                                // green breathing, 3 s period
      uint32_t p   = t % 3000;
      uint32_t tri = (p < 1500) ? p : 3000 - p;   // 0..1500
      uint32_t lvl = tri * 255 / 1500;
      lvl = 4 + (lvl * lvl) / 255 * 251 / 255;    // rough gamma, never fully off
      color = pixel.Color(0, lvl, 0);
      break;
    }
    case EMERGENCY_FAST:                          // red, 100 ms on / 100 ms off
      color = ((t % 200) < 100) ? pixel.Color(255, 0, 0) : 0;
      break;
    case EMERGENCY_SLOW:                          // red, 200 ms on / 1800 ms off
      color = ((t % 2000) < 200) ? pixel.Color(255, 0, 0) : 0;
      break;
    case RECOVERY:                                // solid amber
      color = pixel.Color(255, 100, 0);
      break;
  }

  if (color != lastColor) {
    lastColor = color;
    pixel.setPixelColor(0, color);
    pixel.show();
  }
}
#endif

// ============================================================================
void setup() {
  Serial.begin(DEBUG_BAUD);                       // native USB CDC
  Serial.setTxTimeoutMs(0);                       // never block if no USB host is attached

  pinMode(CONTACT_PIN, INPUT_PULLUP);

#if USE_RS232
  Rs232.begin(BAUD_RATE, SERIAL_CONFIG, RS232_RX_PIN, RS232_TX_PIN);
#endif
#if USE_RS485
  pinMode(RS485_CON_PIN, OUTPUT);
  digitalWrite(RS485_CON_PIN, RS485_RX_ENABLE);   // idle in receive (bus released)
  Rs485.begin(BAUD_RATE, SERIAL_CONFIG, RS485_RX_PIN, RS485_TX_PIN);
#endif

#if ENABLE_STATUS_LED
  pixel.begin();
  pixel.setBrightness(LED_BRIGHTNESS);
  pixel.clear();
  pixel.show();
#endif

  delayMicroseconds(200);                         // let the pull-up settle (setup only)

  const uint32_t now = millis();
  rawOpen      = (digitalRead(CONTACT_PIN) == HIGH);
  rawChangedAt = now;

  Serial.printf("[t=%lu ms] BOOT contact=%s port=%s baud=%d\n", (unsigned long)now,
                rawOpen ? "OPEN" : "CLOSED",
                USE_RS232 && USE_RS485 ? "BOTH" : (USE_RS232 ? "RS232" : "RS485"),
                BAUD_RATE);

  state = rawOpen ? EMERGENCY_FAST : NORMAL;      // so the log reads "X -> X" at boot
  enterState(state, now);
}

void loop() {
  const uint32_t now = millis();

  // ---- Sample contact --------------------------------------------------------
  const bool open = (digitalRead(CONTACT_PIN) == HIGH);
  if (open != rawOpen) {
    rawOpen      = open;
    rawChangedAt = now;
  }
  const uint32_t heldFor         = now - rawChangedAt;
  const bool     openStable      = rawOpen  && heldFor >= OPEN_DEBOUNCE_MS;
  const bool     closedConfirmed = !rawOpen && heldFor >= CLEAR_CONFIRM_MS;
  const uint32_t inState         = now - stateEnteredAt;

  // ---- Transitions -------------------------------------------------------------
  switch (state) {
    case NORMAL:
      if (openStable) enterState(EMERGENCY_FAST, now);
      break;

    case EMERGENCY_FAST:
      if (closedConfirmed)                          enterState(RECOVERY, now);
      else if (inState >= EMERG_FAST_DURATION_MS)   enterState(EMERGENCY_SLOW, now);
      break;

    case EMERGENCY_SLOW:
      if (closedConfirmed) enterState(RECOVERY, now);
      break;

    case RECOVERY:
      if (openStable)                               enterState(EMERGENCY_FAST, now);
      else if (inState >= RECOVERY_DURATION_MS)     enterState(NORMAL, now);
      break;
  }

  // ---- Periodic send -------------------------------------------------------------
  const uint32_t interval = intervalFor(state);
  if (now - lastSendAt >= interval) {
    // Keep a drift-free cadence; resync if we somehow fell more than a period behind.
    lastSendAt = (now - lastSendAt >= 2 * interval) ? now : lastSendAt + interval;
    sendMessage(messageFor(state));
  }

  // ---- Discard anything received (ports are transmit-only) ---------------------
#if USE_RS232
  while (Rs232.available()) Rs232.read();
#endif
#if USE_RS485
  while (Rs485.available()) Rs485.read();
#endif

#if ENABLE_STATUS_LED
  updateLed(now);
#endif
}
