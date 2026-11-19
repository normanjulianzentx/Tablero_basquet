#include <Arduino.h>
#include <Preferences.h>
#include <Update.h>
#include <WebServer.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <driver/rmt.h>
#define FASTLED_RMT_MAX_CHANNELS 1
#include "config.h"
#include "protocol.h"
#include "secrets.h"
#include <FastLED.h>
#include <math.h>

WebServer otaServer(80);
CRGB leds[NUM_LEDS];
Preferences prefs;
uint32_t rxBlinkUntil = 0;
bool isDirty = true;              // Flag de actualización: solo envía pulsos a los LEDs si hubo un cambio real
int32_t lastRenderedSecond = -1;  // Último segundo dibujado en el reloj

// Modo de prueba de encendido total desde Web (T-B-O-T-A)
// 0 = Normal (Partido), 1 = Todo Rojo, 2 = Todo Verde, 3 = Todo Azul, 4 = Full White, 5 = Todos en 8
uint8_t solidTestMode = 0;

#if defined(EMERGENCY_CLOCK_ONLY) && EMERGENCY_CLOCK_ONLY
bool clockRealTimeMode = false;
int8_t clockHour = 12;
int8_t clockMinute = 0;
int8_t clockSecond = 0;
uint32_t lastClockSecTick = 0;
#endif

// ================== DRIVER RMT HARDWARE PURO (SIN BUG GPIO 0x100 DE FASTLED) ==================
// Precalcula los 240 LEDs en RAM y mantiene GPIO 4 clavado en LOW (0.0V) entre tramas,
// evitando que FastLED 3.6.0 conecte GPIO 4 al bus SPI Flash (SPIQ_OUT_IDX = 0) en el ESP32-C3.
static rmt_item32_t rmtItems[NUM_LEDS * 24 + 1];

void initLedsRMT() {
  rmt_config_t rmt_tx = {};
  rmt_tx.channel = RMT_CHANNEL_0;
  rmt_tx.gpio_num = (gpio_num_t)LED_PIN;
  rmt_tx.rmt_mode = RMT_MODE_TX;
  rmt_tx.clk_div = 2; // 80 MHz / 2 = 40 MHz (1 tick = 25 ns)
  rmt_tx.mem_block_num = 2; // Ambos bloques TX del ESP32-C3 (96 palabras)
  rmt_tx.tx_config.loop_en = false;
  rmt_tx.tx_config.carrier_en = false;
  rmt_tx.tx_config.idle_level = RMT_IDLE_LEVEL_LOW;
  rmt_tx.tx_config.idle_output_en = true;
  rmt_config(&rmt_tx);
  rmt_driver_install(RMT_CHANNEL_0, 0, 0);
}

void showLeds() {
  // Calcula escala de brillo respetando LED_BRIGHTNESS y tope de corriente MAX_MILLIAMPS
  uint8_t scale = calculate_max_brightness_for_power_vmA(
      leds, NUM_LEDS, LED_BRIGHTNESS, 5, MAX_MILLIAMPS);

  // Tiempos WS2811 a 40 MHz (1 tick = 25 ns, período = 1.25 us = 50 ticks):
  // Bit 0: 300 ns HIGH (12 ticks), 950 ns LOW (38 ticks)
  // Bit 1: 800 ns HIGH (32 ticks), 450 ns LOW (18 ticks)
  const rmt_item32_t bit0 = {{{12, 1, 38, 0}}};
  const rmt_item32_t bit1 = {{{32, 1, 18, 0}}};

  uint32_t idx = 0;
  for (uint16_t i = 0; i < NUM_LEDS; i++) {
    // Orden físico RGB
    uint8_t r = scale8(leds[i].r, scale);
    uint8_t g = scale8(leds[i].g, scale);
    uint8_t b = scale8(leds[i].b, scale);

    for (int8_t bit = 7; bit >= 0; bit--) {
      rmtItems[idx++] = (r & (1 << bit)) ? bit1 : bit0;
    }
    for (int8_t bit = 7; bit >= 0; bit--) {
      rmtItems[idx++] = (g & (1 << bit)) ? bit1 : bit0;
    }
    for (int8_t bit = 7; bit >= 0; bit--) {
      rmtItems[idx++] = (b & (1 << bit)) ? bit1 : bit0;
    }
  }

  // Pulso final de Reset / Latch en LOW (300 us en 0V para que el último LED cierre perfecto)
  rmtItems[idx].duration0 = 6000;
  rmtItems[idx].level0 = 0;
  rmtItems[idx].duration1 = 6000;
  rmtItems[idx].level1 = 0;
  idx++;

  rmt_write_items(RMT_CHANNEL_0, rmtItems, idx, true);
  rmt_wait_tx_done(RMT_CHANNEL_0, pdMS_TO_TICKS(50));
}

void clearLeds() {
  for (uint16_t i = 0; i < NUM_LEDS; i++) {
    leds[i] = CRGB::Black;
  }
  showLeds();
}

// ---------- Modo Deportivo ----------
SportMode sportMode = DEFAULT_SPORT_MODE;
uint32_t sportIndicatorUntil = 0;

// ---------- Paleta de Colores de Equipos ----------
static const CRGB TEAM_COLORS[] = {
  CRGB(255, 130, 0),   // 0: Ámbar / Naranja dorado
  CRGB(0, 220, 40),    // 1: Verde deportivo
  CRGB(0, 160, 255),   // 2: Celeste / Cyan
  CRGB::Red,           // 3: Rojo
  CRGB::White,         // 4: Blanco
  CRGB(255, 230, 0),   // 5: Amarillo
  CRGB(200, 0, 255)    // 6: Magenta / Violeta
};
static const uint8_t NUM_TEAM_COLORS = sizeof(TEAM_COLORS) / sizeof(TEAM_COLORS[0]);

uint8_t homeColorIdx = 0;   // Por defecto: 0 (Ámbar)
uint8_t awayColorIdx = 1;   // Por defecto: 1 (Verde deportivo)
uint8_t periodColorIdx = 2; // Por defecto: 2 (Celeste / Cyan)
uint8_t timeColorIdx = 1;   // Por defecto: 1 (Verde puro)

CRGB getHomeColor()   { return (homeColorIdx == 0)   ? COLOR_LOCAL  : TEAM_COLORS[homeColorIdx % NUM_TEAM_COLORS]; }
CRGB getAwayColor()   { return (awayColorIdx == 1)   ? COLOR_AWAY   : TEAM_COLORS[awayColorIdx % NUM_TEAM_COLORS]; }
CRGB getPeriodColor() { return (periodColorIdx == 2) ? COLOR_PERIOD : TEAM_COLORS[periodColorIdx % NUM_TEAM_COLORS]; }
CRGB getTimeColor()   { return (timeColorIdx == 1)   ? COLOR_TIME   : TEAM_COLORS[timeColorIdx % NUM_TEAM_COLORS]; }

// ---------- Presets de Tiempo (minutos: incluye 1m, descansos/entretiempos y cuartos) ----------
static const uint8_t TIME_PRESETS[] = {1, 2, 5, 10, 12, 15, 20};
static const uint8_t NUM_TIME_PRESETS = sizeof(TIME_PRESETS) / sizeof(TIME_PRESETS[0]);

// ---------- Estado del juego (el tablero es la única fuente de verdad) ----------
uint8_t homeScore = 0, awayScore = 0, period = 1;
uint8_t homeFouls = 0, awayFouls = 0;
uint8_t configuredPeriodMinutes = 1; // Por defecto 1 minuto (configurable a 1, 2, 5, 10, 12, 15, 20 min)
int32_t remainingMs = 1 * 60 * 1000L;
bool running = false;
bool isHalftime = false; // true = Muestra 'E' (Entretiempo) en el dígito de Período
uint32_t lastTick = 0;
uint32_t hornUntil = 0;
uint32_t finishFlashUntil = 0;
uint32_t lastPeriodDecMs = 0; // Para detectar doble toque rápido de Shift + Período

// ---------- Anti-replay ----------
uint32_t storedSession = 0;
uint32_t lastSeq = 0;

// ---------- Cola de recepción (callback -> loop) ----------
#define RXQ 8
CmdPacket rxQueue[RXQ];
volatile uint8_t qHead = 0, qTail = 0;

// Segmentos A..G -> bits 0..6
// 0=A, 1=B, 2=C, 3=D, 4=E, 5=F, 6=G
static const uint8_t DIGIT_MASK[10] = {
  0b0111111, // 0
  0b0000110, // 1
  0b1011011, // 2
  0b1001111, // 3
  0b1100110, // 4
  0b1101101, // 5
  0b1111101, // 6
  0b0000111, // 7
  0b1111111, // 8
  0b1101111  // 9
};

// ================== Bocina y Buzzer ==================
void setRelay(bool on) { 
  digitalWrite(RELAY_PIN, (on ^ RELAY_ACTIVE_LOW) ? HIGH : LOW); 
}

void beep(uint16_t ms) {
  ledcWriteTone(BUZZER_LEDC_CH, BUZZER_FREQ_HZ);
  delay(ms);
  ledcWriteTone(BUZZER_LEDC_CH, 0);
}

void startHorn(uint32_t ms) {
  if (ms > HORN_MAX_MS) ms = HORN_MAX_MS;
  hornUntil = millis() + ms;
}

bool hornActive() { 
  return (int32_t)(hornUntil - millis()) > 0; 
}

void updateHornOutputs() {
  static bool prev = false;
  bool on = hornActive();
  if (on != prev) {
    prev = on;
    setRelay(on);
    ledcWriteTone(BUZZER_LEDC_CH, on ? BUZZER_FREQ_HZ : 0);
  }
}

// ================== Dibujo de Dígitos y Faltas ==================
void drawDigit(uint16_t offset, int digit, CRGB color) {
  uint8_t mask = 0;
  if (digit >= 0 && digit <= 9) {
    mask = DIGIT_MASK[digit];
  } else if (digit == -2) {
    // 'b' (Básquet): segmentos c, d, e, f, g
    mask = 0b1111100;
  } else if (digit == -3) {
    // 'F' (Futsal): segmentos a, e, f, g
    mask = 0b1110001;
  } else if (digit == -4) {
    // '-' (guion): segmento g
    mask = 0b1000000;
  } else if (digit == -5) {
    // 'E' (Entretiempo): segmentos a, d, e, f, g
    mask = 0b1111001;
  } else {
    mask = 0; // Apagado
  }

  for (uint8_t pos = 0; pos < 7; pos++) {
    bool on = mask & (1 << SEG_WIRING[pos]);
    for (uint8_t k = 0; k < LEDS_PER_SEG; k++) {
      leds[offset + pos * LEDS_PER_SEG + k] = on ? color : CRGB::Black;
    }
  }
}

void drawScore(uint16_t offset, uint8_t score, CRGB color) {
  score %= 100;
  drawDigit(offset, score / 10, color);  // Muestra siempre la decena (00..99)
  drawDigit(offset + LEDS_PER_DIGIT, score % 10, color);
}

void drawFoulBlock(uint16_t offset, CRGB color) {
  for (uint8_t k = 0; k < LEDS_PER_FOUL_BLOCK; k++) {
    leds[offset + k] = color;
  }
}

// Dibuja los bloques de faltas de cada equipo - MODO BÁSQUET EXCLUSIVO
// Faltas 1, 2 y 3 en VERDE PURO; 4 y 5 en ROJO VIVO (penalidad/bonus); 6to bloque (centro) SIEMPRE APAGADO
void drawFouls() {
  // 1. Limpieza preventiva de toda la sección de faltas (48 LEDs: 106..153)
  for (uint16_t i = OFF_FOUL_AWAY; i < OFF_TIME_D1; i++) {
    leds[i] = CRGB::Black;
  }

  // 2. FALTAS VISITA (OFF_FOUL_AWAY = 106..129: Bloques 0..4 avanzan desde el extremo derecho hacia el centro; Bloque 5 es el centro apagado)
  // Bloque 0: Falta 1 (LEDs 106..109, extremo derecho) - VERDE puro
  // Bloque 1: Falta 2 (LEDs 110..113) - VERDE puro
  // Bloque 2: Falta 3 (LEDs 114..117) - VERDE puro
  // Bloque 3: Falta 4 (LEDs 118..121) - ROJO vivo (penalidad)
  // Bloque 4: Falta 5 (LEDs 122..125) - ROJO vivo (penalidad)
  // Bloque 5: LEDs 126..129 (6to bloque - CENTRO) -> DIRECTAMENTE APAGADO SIEMPRE
  for (uint8_t f = 0; f < 5; f++) {
    if (awayFouls > f) {
      CRGB col = (f >= 3) ? COLOR_FOUL_PENALTY : COLOR_FOUL_NORMAL;
      drawFoulBlock(OFF_FOUL_AWAY + f * LEDS_PER_FOUL_BLOCK, col);
    }
  }

  // 3. FALTAS LOCAL (OFF_FOUL_HOME = 130..153: Bloques 5..1 avanzan desde el extremo izquierdo hacia el centro; Bloque 0 es el centro apagado)
  // Bloque 5: Falta 1 (LEDs 150..153, extremo izquierdo) - VERDE puro
  // Bloque 4: Falta 2 (LEDs 146..149) - VERDE puro
  // Bloque 3: Falta 3 (LEDs 142..145) - VERDE puro
  // Bloque 2: Falta 4 (LEDs 138..141) - ROJO vivo (penalidad)
  // Bloque 1: Falta 5 (LEDs 134..137) - ROJO vivo (penalidad)
  // Bloque 0: LEDs 130..133 (6to bloque - CENTRO) -> DIRECTAMENTE APAGADO SIEMPRE
  for (uint8_t f = 0; f < 5; f++) {
    if (homeFouls > f) {
      CRGB col = (f >= 3) ? COLOR_FOUL_PENALTY : COLOR_FOUL_NORMAL;
      uint8_t block = 5 - f; // f=0 (falta 1) -> bloque 5 (extremo izq); f=4 (falta 5) -> bloque 1 (junto al centro)
      drawFoulBlock(OFF_FOUL_HOME + block * LEDS_PER_FOUL_BLOCK, col);
    }
  }

  // 4. Asegurar directamente apagados los 8 LEDs del centro del tablero (Bloque 5 de Visita y Bloque 0 de Local)
  drawFoulBlock(OFF_FOUL_AWAY + 5 * LEDS_PER_FOUL_BLOCK, CRGB::Black); // LEDs 126..129 (4 LEDs centro Visita)
  drawFoulBlock(OFF_FOUL_HOME + 0 * LEDS_PER_FOUL_BLOCK, CRGB::Black); // LEDs 130..133 (4 LEDs centro Local)
}

// Dos puntos del reloj (Siempre visibles en VERDE deportivo)
void drawColon(CRGB color) {
  for (uint8_t i = 0; i < 2 * LEDS_PER_COLON_DOT; i++) {
    leds[OFF_TIME_COLON + i] = color;
  }
}

void drawTime() {
#if defined(EMERGENCY_CLOCK_ONLY) && EMERGENCY_CLOCK_ONLY
  if (clockRealTimeMode) {
    int d1 = (clockHour % 24) / 10;
    int d2 = (clockHour % 24) % 10;
    int d3 = (clockMinute % 60) / 10;
    int d4 = (clockMinute % 60) % 10;
    CRGB color = getTimeColor();
    drawDigit(OFF_TIME_D1, d1, color);
    drawDigit(OFF_TIME_D2, d2, color);
    drawDigit(OFF_TIME_D3, d3, color);
    drawDigit(OFF_TIME_D4, d4, color);
    drawColon((clockSecond % 2 == 0) ? color : CRGB::Black);
    return;
  }
#endif

  int32_t ms = remainingMs < 0 ? 0 : remainingMs;
  uint32_t now = millis();

  // Al terminar el tiempo: 00:00 parpadeando
  bool finishing = (ms == 0 && (int32_t)(finishFlashUntil - now) > 0);
  if (finishing && ((now / 250) % 2)) {
    for (uint16_t i = OFF_TIME_D1; i < NUM_LEDS; i++) leds[i] = CRGB::Black;
    return;
  }

  CRGB color = getTimeColor(); // Color configurable de TIEMPO (Verde puro por defecto)
  bool low = (ms < 60000); // Último minuto (< 60 segundos): cambio a Décimas de segundo

  int d1, d2, d3, d4;
  if (!low) {
    // MM:SS (Minutos y Segundos)
    uint32_t s = ms / 1000;
    d1 = (s / 60) / 10;
    d2 = (s / 60) % 10;
    d3 = (s % 60) / 10;
    d4 = (s % 60) % 10;
  } else {
    // SS:dc (Último minuto: Segundos y Décimas de segundo)
    uint32_t ss = ms / 1000;
    uint32_t cc = (ms % 1000) / 10; // centésimas 00..99
    d1 = ss / 10;
    d2 = ss % 10;
    d3 = cc / 10;                    // Décimas 0..9
    d4 = cc % 10;                    // Centésimas 0..9
  }

  drawDigit(OFF_TIME_D1, d1, color);
  drawDigit(OFF_TIME_D2, d2, color);
  drawDigit(OFF_TIME_D3, d3, color);
  drawDigit(OFF_TIME_D4, d4, color);
  drawColon(color);
}

void render() {
  // Modo de prueba de color fijo sobre todo el tablero (pedido desde T-B-O-T-A)
  if (solidTestMode > 0) {
    if (solidTestMode == 5) {
      // Todos los dígitos en 8 y faltas activas con colores de juego actuales
      drawScore(OFF_HOME, 88, getHomeColor());
      drawDigit(OFF_PERIOD, 8, getPeriodColor());
      drawScore(OFF_AWAY, 88, getAwayColor());
      for (uint8_t f = 0; f < 5; f++) {
        CRGB col = (f >= 3) ? COLOR_FOUL_PENALTY : COLOR_FOUL_NORMAL;
        drawFoulBlock(OFF_FOUL_AWAY + f * LEDS_PER_FOUL_BLOCK, col);
        drawFoulBlock(OFF_FOUL_HOME + (5 - f) * LEDS_PER_FOUL_BLOCK, col);
      }
      drawFoulBlock(OFF_FOUL_AWAY + 5 * LEDS_PER_FOUL_BLOCK, CRGB::Black);
      drawFoulBlock(OFF_FOUL_HOME + 0 * LEDS_PER_FOUL_BLOCK, CRGB::Black);
      CRGB tCol = getTimeColor();
      drawDigit(OFF_TIME_D1, 8, tCol);
      drawDigit(OFF_TIME_D2, 8, tCol);
      drawDigit(OFF_TIME_D3, 8, tCol);
      drawDigit(OFF_TIME_D4, 8, tCol);
      drawColon(tCol);
    } else {
      CRGB testCol = CRGB::Black;
      if (solidTestMode == 1) testCol = CRGB(255, 0, 0);       // ROJO PURO
      else if (solidTestMode == 2) testCol = CRGB(0, 255, 0);  // VERDE PURO
      else if (solidTestMode == 3) testCol = CRGB(0, 0, 255);  // AZUL PURO
      else if (solidTestMode == 4) testCol = CRGB(255, 255, 255); // FULL WHITE
      for (uint16_t i = BUFFER_LED_COUNT; i < NUM_LEDS; i++) {
        leds[i] = testCol;
      }
    }
#if defined(BUFFER_LED_COUNT) && BUFFER_LED_COUNT > 0
    for (uint8_t i = 0; i < BUFFER_LED_COUNT; i++) leds[i] = CRGB::Black;
#endif
    showLeds();
    isDirty = false;
    return;
  }

#if defined(EMERGENCY_CLOCK_ONLY) && EMERGENCY_CLOCK_ONLY
  // MODO EMERGENCIA: Todo apagado excepto el reloj
  for (uint16_t i = 0; i < OFF_TIME_D1; i++) {
    leds[i] = CRGB::Black;
  }
  drawTime();
#if defined(BUFFER_LED_COUNT) && BUFFER_LED_COUNT > 0
  for (uint8_t i = 0; i < BUFFER_LED_COUNT; i++) leds[i] = CRGB::Black;
#endif
  showLeds();
  isDirty = false;
  return;
#endif

  drawScore(OFF_HOME, homeScore, getHomeColor());
  drawDigit(OFF_PERIOD, isHalftime ? -5 : period, getPeriodColor()); // -5 dibuja la letra 'E' en Entretiempo
  drawScore(OFF_AWAY, awayScore, getAwayColor());
  drawFouls();
  drawTime();
#if defined(BUFFER_LED_COUNT) && BUFFER_LED_COUNT > 0
  for (uint8_t i = 0; i < BUFFER_LED_COUNT; i++) leds[i] = CRGB::Black;
#endif
  showLeds();
  isDirty = false;
}

void bootSequence() {
  clearLeds();
}

// Reinicio completo de partido a cero (00-00, Período 1, Faltas 0-0, Tiempo configurado en pausa)
void resetFullGame() {
  running = false;
  isHalftime = false;
  homeScore = 0;
  awayScore = 0;
  period = 1;
  homeFouls = 0;
  awayFouls = 0;
  remainingMs = (int32_t)configuredPeriodMinutes * 60 * 1000L;
  finishFlashUntil = 0;
  hornUntil = 0;
  setRelay(false);
  beep(100);
  delay(80);
  beep(150);
}

// ================== Comandos Remotos ==================
void applyCommand(const CmdPacket &p) {
  // Si estaba en prueba de color fijo y se toca el control, vuelve al partido
  if (p.cmd != CMD_SYNC && solidTestMode != 0) {
    solidTestMode = 0;
  }

  switch (p.cmd) {
    case CMD_SYNC: break;
    case CMD_TIMER_TOGGLE:
      if (remainingMs > 0) { 
        running = !running; 
        lastTick = millis(); 
      }
      break;
    case CMD_HOME_INC: if (homeScore < 99) homeScore++; break;
    case CMD_HOME_DEC: if (homeScore > 0) homeScore--; break;
    case CMD_AWAY_INC: if (awayScore < 99) awayScore++; break;
    case CMD_AWAY_DEC: if (awayScore > 0) awayScore--; break;
    
    case CMD_PERIOD_INC: 
      if (isHalftime) {
        // Si estaba en Entretiempo ('E'), al tocar +Período sale de 'E' y avanza al siguiente cuarto
        isHalftime = false;
      }
      period = (period % MAX_PERIOD) + 1; 
      homeFouls = 0;
      awayFouls = 0;
      running = false;
      remainingMs = (int32_t)configuredPeriodMinutes * 60 * 1000L;
      break;
    case CMD_PERIOD_DEC: {
      uint32_t nowMs = millis();
      isHalftime = false;
      // Si está en Período 1, o si se presiona Shift + Período 2 veces seguidas (< 2.5s):
      // Reinicia TODO el partido a cero (00-00, Período 1, Faltas 0-0, Reloj recargado)
      if (period <= 1 || (nowMs - lastPeriodDecMs < 2500)) {
        lastPeriodDecMs = 0;
        resetFullGame();
      } else {
        lastPeriodDecMs = nowMs;
        period--;
        homeFouls = 0;
        awayFouls = 0;
        running = false;
        remainingMs = (int32_t)configuredPeriodMinutes * 60 * 1000L;
      }
      break;
    }

    case CMD_FOUL_HOME_INC: {
      if (homeFouls < BASKET_MAX_FOULS) {
        homeFouls++;
        if (homeFouls >= 4) beep(150);
      }
      break;
    }
    case CMD_FOUL_HOME_DEC: 
      if (homeFouls > 0) homeFouls--; 
      break;

    case CMD_FOUL_AWAY_INC: {
      if (awayFouls < BASKET_MAX_FOULS) {
        awayFouls++;
        if (awayFouls >= 4) beep(150);
      }
      break;
    }
    case CMD_FOUL_AWAY_DEC: 
      if (awayFouls > 0) awayFouls--; 
      break;

    case CMD_TIME_ADD: {
      int32_t t = remainingMs + (int32_t)p.value * 1000L;
      remainingMs = constrain(t, 0, 99L * 60 * 1000 + 59000);
      break;
    }
    case CMD_TIME_RESET:
      running = false;
      isHalftime = false;
      remainingMs = (int32_t)configuredPeriodMinutes * 60 * 1000L;
      break;

    case CMD_GAME_RESET:
    case CMD_TEST_MODE:
    case CMD_SPORT_TOGGLE:
    case CMD_SET_SPORT_MODE:
      sportMode = SPORT_BASKET;
      resetFullGame();
      break;

    case CMD_HORN:
      if (p.value >= 100) startHorn(p.value);
      break;

    case CMD_COLOR_CYCLE_HOME:
      if (p.value >= 1 && p.value <= NUM_TEAM_COLORS) {
        homeColorIdx = (uint8_t)(p.value - 1);
      } else {
        homeColorIdx = (homeColorIdx + 1) % NUM_TEAM_COLORS;
      }
      prefs.putUChar("col_home", homeColorIdx);
      beep(70);
      break;

    case CMD_COLOR_CYCLE_AWAY:
      if (p.value >= 1 && p.value <= NUM_TEAM_COLORS) {
        awayColorIdx = (uint8_t)(p.value - 1);
      } else {
        awayColorIdx = (awayColorIdx + 1) % NUM_TEAM_COLORS;
      }
      prefs.putUChar("col_away", awayColorIdx);
      beep(70);
      break;

    case CMD_COLOR_CYCLE_PERIOD:
      if (p.value >= 1 && p.value <= NUM_TEAM_COLORS) {
        periodColorIdx = (uint8_t)(p.value - 1);
      } else {
        periodColorIdx = (periodColorIdx + 1) % NUM_TEAM_COLORS;
      }
      prefs.putUChar("col_per", periodColorIdx);
      beep(70);
      break;

    case CMD_COLOR_CYCLE_TIME:
      if (p.value >= 1 && p.value <= NUM_TEAM_COLORS) {
        timeColorIdx = (uint8_t)(p.value - 1);
      } else {
        timeColorIdx = (timeColorIdx + 1) % NUM_TEAM_COLORS;
      }
      prefs.putUChar("col_time", timeColorIdx);
      beep(70);
      break;

    case CMD_HALFTIME_TOGGLE: {
      running = false;
      isHalftime = !isHalftime;
      if (isHalftime) {
        int32_t sec = (p.value > 0) ? p.value : 60; // Por defecto 1 minuto de entretiempo ('E')
        remainingMs = sec * 1000L;
      } else {
        remainingMs = (int32_t)configuredPeriodMinutes * 60 * 1000L;
      }
      beep(90);
      break;
    }

    case CMD_TIME_CYCLE_PRESET: {
      running = false;
      isHalftime = false;
      uint32_t currentMin = remainingMs / 60000;
      uint8_t nextIdx = 0;
      for (uint8_t i = 0; i < NUM_TIME_PRESETS; i++) {
        if (TIME_PRESETS[i] == currentMin) {
          nextIdx = (i + 1) % NUM_TIME_PRESETS;
          break;
        } else if (TIME_PRESETS[i] > currentMin) {
          nextIdx = i;
          break;
        }
      }
      configuredPeriodMinutes = TIME_PRESETS[nextIdx];
      prefs.putUChar("period_min", configuredPeriodMinutes);
      remainingMs = (int32_t)configuredPeriodMinutes * 60 * 1000L;
      beep(90);
      break;
    }

    case CMD_SET_EXACT_SECONDS: {
      running = false;
      if (p.value < 0) {
        // Valor negativo = Modo Entretiempo ('E' en dígito de Período)
        isHalftime = true;
        remainingMs = (int32_t)constrain(-p.value, 1, 3600) * 1000L;
      } else {
        // Valor positivo = Tiempo de cuarto normal
        isHalftime = false;
        remainingMs = (int32_t)constrain(p.value, 0, 3600) * 1000L;
        if (p.value >= 60) {
          configuredPeriodMinutes = p.value / 60;
          prefs.putUChar("period_min", configuredPeriodMinutes);
        }
      }
      beep(100);
      break;
    }

    default: break;
  }
}

void sendState(uint32_t ackSession, uint32_t ackSeq) {
  StatePacket s = {};
  s.magic = PROTOCOL_MAGIC;
  s.version = PROTOCOL_VERSION;
  s.type = PKT_STATE;
  s.home = homeScore; 
  s.away = awayScore; 
  s.period = period;
  s.homeFouls = homeFouls; 
  s.awayFouls = awayFouls;
  s.flags = (running ? STATE_RUNNING : 0) 
          | (hornActive() ? STATE_HORN : 0)
          | ((sportMode == SPORT_FUTSAL) ? STATE_FUTSAL_MODE : 0)
          | (isHalftime ? STATE_HALFTIME_MODE : 0);
  s.remainingMs = remainingMs < 0 ? 0 : remainingMs;
  s.ackSession = ackSession;
  s.ackSeq = ackSeq;
  sealPacket(s);
  esp_now_send(REMOTE_MAC, (uint8_t*)&s, sizeof(s));
}

void handleCommand(const CmdPacket &p) {
  if (p.session < storedSession) return;
  if (p.session > storedSession) {
    storedSession = p.session;
    lastSeq = 0;
    prefs.putUInt("session", storedSession);
  }
  if (p.seq > lastSeq) {
    lastSeq = p.seq;
    applyCommand(p);
    isDirty = true;
  }
  
  digitalWrite(STATUS_LED, HIGH);
  rxBlinkUntil = millis() + 50;

  sendState(p.session, p.seq);
}

// Callback ESP-NOW
void onReceive(const uint8_t *mac, const uint8_t *data, int len) {
  CmdPacket p;
  if (!parsePacket(data, len, PKT_COMMAND, p)) {
    return;
  }
  uint8_t next = (qHead + 1) % RXQ;
  if (next == qTail) return;
  rxQueue[qHead] = p;
  qHead = next;
}

void setupEspNow() {
  WiFi.mode(WIFI_AP_STA);
  WiFi.disconnect();
  WiFi.softAP("T-B-O-T-A", "12345678", ESPNOW_CHANNEL, 0, 2);
  esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
  esp_wifi_set_max_tx_power(WIFI_POWER_8_5dBm);
  WiFi.setSleep(false);
  Serial.print("MAC tablero: "); Serial.println(WiFi.macAddress());
  Serial.print("OTA Tablero SSID: T-B-O-T-A  IP: "); Serial.println(WiFi.softAPIP());

  // Servidor Web + Diagnóstico de Color + OTA
  auto handleUpdatePage = []() {
    otaServer.send(200, "text/html; charset=utf-8",
      "<!DOCTYPE html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
      "<title>Tablero Deportivo - Diagnóstico y OTA</title>"
      "<style>body{background:#090d16;color:#f8fafc;font-family:sans-serif;padding:14px;text-align:center;}"
      ".card{background:#131b2e;border:1px solid #1e293b;border-radius:14px;padding:16px;max-width:440px;margin:14px auto;box-shadow:0 4px 14px rgba(0,0,0,0.5);}"
      "h2{margin:0 0 10px;font-size:1.15rem;}"
      ".grid2{display:grid;grid-template-columns:1fr 1fr;gap:8px;margin-bottom:8px;}"
      "button{border:none;padding:12px 10px;border-radius:8px;font-weight:bold;cursor:pointer;font-size:0.95rem;width:100%;color:#fff;}"
      "input[type=file]{margin:14px 0;width:100%;color:#94a3b8;font-size:0.9rem;}"
      "input[type=submit]{background:#059669;color:#fff;border:none;padding:12px 24px;border-radius:8px;font-weight:bold;cursor:pointer;width:100%;font-size:1rem;}"
      "#prg{display:none;margin-top:14px;font-weight:bold;color:#34d399;}"
      "</style></head><body>"
      "<div class='card' style='border-color:#f59e0b;'>"
      "<h2 style='color:#fbbf24;'>🎨 Personalización de Colores</h2>"
      "<p style='margin:4px 0 6px;font-size:0.85rem;color:#cbd5e1;'><b>LOCAL</b> (o mantener botón LOCAL 1.5s):</p>"
      "<div style='display:flex;gap:6px;justify-content:center;margin-bottom:10px;'>"
      "<button onclick=\"fetch('/team_color?t=h&c=0')\" style='background:#ff8200;height:36px;' title='Ámbar'></button>"
      "<button onclick=\"fetch('/team_color?t=h&c=1')\" style='background:#00dc28;height:36px;' title='Verde'></button>"
      "<button onclick=\"fetch('/team_color?t=h&c=2')\" style='background:#00a0ff;height:36px;' title='Celeste'></button>"
      "<button onclick=\"fetch('/team_color?t=h&c=3')\" style='background:#ff0000;height:36px;' title='Rojo'></button>"
      "<button onclick=\"fetch('/team_color?t=h&c=4')\" style='background:#ffffff;height:36px;border:1px solid #64748b;' title='Blanco'></button>"
      "<button onclick=\"fetch('/team_color?t=h&c=5')\" style='background:#ffe600;height:36px;' title='Amarillo'></button>"
      "<button onclick=\"fetch('/team_color?t=h&c=6')\" style='background:#c800ff;height:36px;' title='Violeta'></button>"
      "</div>"
      "<p style='margin:4px 0 6px;font-size:0.85rem;color:#cbd5e1;'><b>VISITA</b> (o mantener botón VISITA 1.5s):</p>"
      "<div style='display:flex;gap:6px;justify-content:center;margin-bottom:10px;'>"
      "<button onclick=\"fetch('/team_color?t=a&c=0')\" style='background:#ff8200;height:36px;' title='Ámbar'></button>"
      "<button onclick=\"fetch('/team_color?t=a&c=1')\" style='background:#00dc28;height:36px;' title='Verde'></button>"
      "<button onclick=\"fetch('/team_color?t=a&c=2')\" style='background:#00a0ff;height:36px;' title='Celeste'></button>"
      "<button onclick=\"fetch('/team_color?t=a&c=3')\" style='background:#ff0000;height:36px;' title='Rojo'></button>"
      "<button onclick=\"fetch('/team_color?t=a&c=4')\" style='background:#ffffff;height:36px;border:1px solid #64748b;' title='Blanco'></button>"
      "<button onclick=\"fetch('/team_color?t=a&c=5')\" style='background:#ffe600;height:36px;' title='Amarillo'></button>"
      "<button onclick=\"fetch('/team_color?t=a&c=6')\" style='background:#c800ff;height:36px;' title='Violeta'></button>"
      "</div>"
      "<p style='margin:4px 0 6px;font-size:0.85rem;color:#cbd5e1;'><b>PERÍODO</b>:</p>"
      "<div style='display:flex;gap:6px;justify-content:center;margin-bottom:10px;'>"
      "<button onclick=\"fetch('/team_color?t=p&c=0')\" style='background:#ff8200;height:36px;' title='Ámbar'></button>"
      "<button onclick=\"fetch('/team_color?t=p&c=1')\" style='background:#00dc28;height:36px;' title='Verde'></button>"
      "<button onclick=\"fetch('/team_color?t=p&c=2')\" style='background:#00a0ff;height:36px;' title='Celeste'></button>"
      "<button onclick=\"fetch('/team_color?t=p&c=3')\" style='background:#ff0000;height:36px;' title='Rojo'></button>"
      "<button onclick=\"fetch('/team_color?t=p&c=4')\" style='background:#ffffff;height:36px;border:1px solid #64748b;' title='Blanco'></button>"
      "<button onclick=\"fetch('/team_color?t=p&c=5')\" style='background:#ffe600;height:36px;' title='Amarillo'></button>"
      "<button onclick=\"fetch('/team_color?t=p&c=6')\" style='background:#c800ff;height:36px;' title='Violeta'></button>"
      "</div>"
      "<p style='margin:4px 0 6px;font-size:0.85rem;color:#cbd5e1;'><b>TIEMPO (Reloj)</b>:</p>"
      "<div style='display:flex;gap:6px;justify-content:center;'>"
      "<button onclick=\"fetch('/team_color?t=m&c=0')\" style='background:#ff8200;height:36px;' title='Ámbar'></button>"
      "<button onclick=\"fetch('/team_color?t=m&c=1')\" style='background:#00dc28;height:36px;' title='Verde'></button>"
      "<button onclick=\"fetch('/team_color?t=m&c=2')\" style='background:#00a0ff;height:36px;' title='Celeste'></button>"
      "<button onclick=\"fetch('/team_color?t=m&c=3')\" style='background:#ff0000;height:36px;' title='Rojo'></button>"
      "<button onclick=\"fetch('/team_color?t=m&c=4')\" style='background:#ffffff;height:36px;border:1px solid #64748b;' title='Blanco'></button>"
      "<button onclick=\"fetch('/team_color?t=m&c=5')\" style='background:#ffe600;height:36px;' title='Amarillo'></button>"
      "<button onclick=\"fetch('/team_color?t=m&c=6')\" style='background:#c800ff;height:36px;' title='Violeta'></button>"
      "</div>"
      "</div>"
      "<div class='card' style='border-color:#3b82f6;'>"
      "<h2 style='color:#60a5fa;'>💡 Prueba de Encendido Total (239 LEDs)</h2>"
      "<div class='grid2'>"
      "<button onclick=\"fetch('/test_color?c=red')\" style='background:#dc2626;'>🔴 TODO ROJO</button>"
      "<button onclick=\"fetch('/test_color?c=green')\" style='background:#16a34a;'>🟢 TODO VERDE</button>"
      "<button onclick=\"fetch('/test_color?c=blue')\" style='background:#2563eb;'>🔵 TODO AZUL</button>"
      "<button onclick=\"fetch('/test_color?c=white')\" style='background:#e2e8f0;color:#0f172a;'>⚪ FULL WHITE</button>"
      "</div>"
      "<button onclick=\"fetch('/test_color?c=eights')\" style='background:#7c3aed;margin-bottom:8px;'>🔢 TODOS LOS DÍGITOS EN 8</button>"
      "<button onclick=\"fetch('/test_color?c=off')\" style='background:#059669;'>🏀 VOLVER A MODO PARTIDO</button>"
      "</div>"
      "<div class='card' style='border-color:#10b981;'>"
      "<h2 style='color:#34d399;'>⏱️ Control de Partido y Entretiempos</h2>"
      "<button onclick=\"fetch('/toggle_timer')\" style='background:#10b981;font-size:1.05rem;margin-bottom:8px;'>▶ / ⏸ START / PAUSA</button>"
      "<div class='grid2'>"
      "<button onclick=\"fetch('/reset_timer')\" style='background:#3b82f6;'>🔄 RECARGAR CUARTO</button>"
      "<button onclick=\"fetch('/horn')\" style='background:#ef4444;'>📢 BOCINA</button>"
      "</div>"
      "<p style='margin:8px 0 4px;font-size:0.82rem;color:#cbd5e1;'><b>SELECCIONAR DURACIÓN DE CUARTO (Fija el tiempo del partido):</b></p>"
      "<div class='grid2'>"
      "<button onclick=\"fetch('/set_time?m=1&save=1')\" style='background:#0284c7;'>🏀 1 Minuto (01:00)</button>"
      "<button onclick=\"fetch('/set_time?m=10&save=1')\" style='background:#1e293b;border:1px solid #38bdf8;'>🏀 10 Min (FIBA)</button>"
      "<button onclick=\"fetch('/set_time?m=12&save=1')\" style='background:#1e293b;'>🏀 12 Min (NBA)</button>"
      "<button onclick=\"fetch('/set_time?m=20&save=1')\" style='background:#1e293b;'>⏱️ 20 Minutos</button>"
      "</div>"
      "<p style='margin:8px 0 4px;font-size:0.82rem;color:#cbd5e1;'><b>ACTIVAR ENTRETIEMPO 'E' (Muestra 'E' en Período):</b></p>"
      "<div class='grid2'>"
      "<button onclick=\"fetch('/set_time?m=1&save=0')\" style='background:#4338ca;'>☕ E - 1 Min</button>"
      "<button onclick=\"fetch('/set_time?m=2&save=0')\" style='background:#4338ca;'>☕ E - 2 Min</button>"
      "<button onclick=\"fetch('/set_time?m=5&save=0')\" style='background:#4338ca;'>☕ E - 5 Min</button>"
      "<button onclick=\"fetch('/set_time?m=15&save=0')\" style='background:#4338ca;'>☕ E - 15 Min</button>"
      "</div>"
      "<button onclick=\"if(confirm('¿Reiniciar TODO el partido a cero (00-00, P1, Faltas 0)?')) fetch('/reset_game');\" style='background:#b91c1c;margin:6px 0 8px;font-size:0.95rem;'>🔄 NUEVO PARTIDO: RESETEAR TODO A CERO (00-00)</button>"
      "<button onclick=\"var d=new Date();fetch('/set_realtime?h='+d.getHours()+'&m='+d.getMinutes()+'&s='+d.getSeconds()).then(()=>alert('Hora sincronizada: '+d.getHours()+':'+d.getMinutes()));\" style='background:#8b5cf6;margin-bottom:6px;font-size:0.9rem;'>🕒 Poner Hora Actual del Celular</button>"
      "</div>"
      "<div class='card'><h2>📦 Actualización Firmware (OTA)</h2>"
      "<p style='color:#94a3b8;font-size:0.85rem;'>Selecciona el archivo binario <b>.bin</b> para flashear:</p>"
      "<form method='POST' action='/update' enctype='multipart/form-data' onsubmit=\"document.getElementById('prg').style.display='block';\">"
      "<input type='file' name='update' accept='.bin' required><br>"
      "<input type='submit' value='Subir y Flashear Tablero'>"
      "</form><div id='prg'>⏳ Flasheando tablero... Espera unos segundos sin cortar la corriente.</div>"
      "</div></body></html>");
  };

  otaServer.on("/", HTTP_GET, handleUpdatePage);
  otaServer.on("/update", HTTP_GET, handleUpdatePage);

  otaServer.on("/test_color", HTTP_GET, []() {
    String c = otaServer.arg("c");
    if (c == "red") solidTestMode = 1;
    else if (c == "green") solidTestMode = 2;
    else if (c == "blue") solidTestMode = 3;
    else if (c == "white") solidTestMode = 4;
    else if (c == "eights") solidTestMode = 5;
    else solidTestMode = 0;
    isDirty = true;
    otaServer.send(200, "text/plain", "OK");
  });

  otaServer.on("/team_color", HTTP_GET, []() {
    solidTestMode = 0;
    String t = otaServer.arg("t");
    uint8_t idx = otaServer.hasArg("c") ? (otaServer.arg("c").toInt() % NUM_TEAM_COLORS) : 0;
    if (t == "h") {
      homeColorIdx = idx;
      prefs.putUChar("col_home", homeColorIdx);
    } else if (t == "a") {
      awayColorIdx = idx;
      prefs.putUChar("col_away", awayColorIdx);
    } else if (t == "p") {
      periodColorIdx = idx;
      prefs.putUChar("col_per", periodColorIdx);
    } else if (t == "m") {
      timeColorIdx = idx;
      prefs.putUChar("col_time", timeColorIdx);
    }
    beep(70);
    isDirty = true;
    otaServer.send(200, "text/plain", "OK");
  });

  otaServer.on("/toggle_timer", HTTP_GET, []() {
    solidTestMode = 0;
#if defined(EMERGENCY_CLOCK_ONLY) && EMERGENCY_CLOCK_ONLY
    clockRealTimeMode = false;
#endif
    if (remainingMs > 0) {
      running = !running;
      lastTick = millis();
      isDirty = true;
    }
    otaServer.send(200, "text/plain", running ? "RUNNING" : "PAUSED");
  });

  otaServer.on("/reset_timer", HTTP_GET, []() {
    solidTestMode = 0;
#if defined(EMERGENCY_CLOCK_ONLY) && EMERGENCY_CLOCK_ONLY
    clockRealTimeMode = false;
#endif
    running = false;
    isHalftime = false;
    remainingMs = (int32_t)configuredPeriodMinutes * 60 * 1000L;
    isDirty = true;
    otaServer.send(200, "text/plain", "RESET");
  });

  otaServer.on("/reset_game", HTTP_GET, []() {
    solidTestMode = 0;
#if defined(EMERGENCY_CLOCK_ONLY) && EMERGENCY_CLOCK_ONLY
    clockRealTimeMode = false;
#endif
    resetFullGame();
    isDirty = true;
    otaServer.send(200, "text/plain", "GAME_RESET");
  });

  otaServer.on("/set_time", HTTP_GET, []() {
    solidTestMode = 0;
#if defined(EMERGENCY_CLOCK_ONLY) && EMERGENCY_CLOCK_ONLY
    clockRealTimeMode = false;
#endif
    uint8_t m = otaServer.hasArg("m") ? constrain(otaServer.arg("m").toInt(), 1, 99) : 1;
    bool saveAsQuarter = (!otaServer.hasArg("save") || otaServer.arg("save").toInt() != 0);
    running = false;
    remainingMs = (int32_t)m * 60 * 1000L;
    if (saveAsQuarter) {
      isHalftime = false;
      configuredPeriodMinutes = m;
      prefs.putUChar("period_min", configuredPeriodMinutes);
    } else {
      isHalftime = true; // Modo Entretiempo: muestra 'E' en el dígito de Período
    }
    beep(90);
    isDirty = true;
    otaServer.send(200, "text/plain", "TIME_SET");
  });

  otaServer.on("/horn", HTTP_GET, []() {
    startHorn(1500);
    otaServer.send(200, "text/plain", "HORN");
  });

  otaServer.on("/set_realtime", HTTP_GET, []() {
    solidTestMode = 0;
#if defined(EMERGENCY_CLOCK_ONLY) && EMERGENCY_CLOCK_ONLY
    if (otaServer.hasArg("h") && otaServer.hasArg("m")) {
      clockHour = otaServer.arg("h").toInt();
      clockMinute = otaServer.arg("m").toInt();
      clockSecond = otaServer.hasArg("s") ? otaServer.arg("s").toInt() : 0;
      clockRealTimeMode = true;
      running = false;
      lastClockSecTick = millis();
      isDirty = true;
      otaServer.send(200, "text/plain", "OK");
      return;
    }
#endif
    otaServer.send(200, "text/plain", "OK");
  });

  otaServer.on("/set_game_mode", HTTP_GET, []() {
    solidTestMode = 0;
#if defined(EMERGENCY_CLOCK_ONLY) && EMERGENCY_CLOCK_ONLY
    clockRealTimeMode = false;
    running = false;
    remainingMs = (int32_t)configuredPeriodMinutes * 60 * 1000L;
    isDirty = true;
#endif
    otaServer.send(200, "text/plain", "OK");
  });

  otaServer.on("/update", HTTP_POST, []() {
    otaServer.sendHeader("Connection", "close");
    otaServer.send(200, "text/html; charset=utf-8",
      Update.hasError() ? "<h3>❌ Error en actualización</h3><p>Verifica el archivo .bin</p><a href='/update'>Reintentar</a>"
                        : "<h3>✅ ¡Actualización de Tablero Exitosa!</h3><p>Reiniciando el tablero...</p><script>setTimeout(()=>window.location.href='/', 6000);</script>");
    delay(500);
    ESP.restart();
  }, []() {
    HTTPUpload& upload = otaServer.upload();
    if (upload.status == UPLOAD_FILE_START) {
      Serial.printf("[OTA-BOARD] Inicio: %s\n", upload.filename.c_str());
      if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
        Update.printError(Serial);
      }
    } else if (upload.status == UPLOAD_FILE_WRITE) {
      if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
        Update.printError(Serial);
      }
    } else if (upload.status == UPLOAD_FILE_END) {
      if (Update.end(true)) {
        Serial.printf("[OTA-BOARD] Completado: %u bytes\n", upload.totalSize);
      } else {
        Update.printError(Serial);
      }
    }
  });

  otaServer.begin();
  
  if (esp_now_init() != ESP_OK) { 
    Serial.println("ERROR crítico: fallo al inicializar ESP-NOW"); 
    return; 
  }
  
  esp_now_set_pmk(ESPNOW_PMK);
  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, REMOTE_MAC, 6);
  memcpy(peer.lmk, ESPNOW_LMK, 16);
  peer.channel = ESPNOW_CHANNEL;
  peer.encrypt = true;
  
  esp_next:
  esp_err_t res = esp_now_add_peer(&peer);
  if (res != ESP_OK) {
    Serial.printf("Error al agregar peer remoto (codigo %d)!\n", res);
  } else {
    Serial.println("Peer remoto registrado con exito (cifrado AES activo)");
  }
  esp_now_register_recv_cb(onReceive);
}

// ================== Inicialización y Bucle Principal ==================
void setup() {
  Serial.begin(115200);
  delay(500);

  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  pinMode(STATUS_LED, OUTPUT);
  digitalWrite(STATUS_LED, LOW);
#if defined(PIN_BOARD_TEST) && PIN_BOARD_TEST != 255
  pinMode(PIN_BOARD_TEST, INPUT_PULLUP);
#endif
  pinMode(RELAY_PIN, OUTPUT);
  setRelay(false);
  ledcSetup(BUZZER_LEDC_CH, BUZZER_FREQ_HZ, 8);
  ledcAttachPin(BUZZER_PIN, BUZZER_LEDC_CH);
  ledcWriteTone(BUZZER_LEDC_CH, 0);

  prefs.begin("tablero", false);
  storedSession = prefs.getUInt("session", 0);
  
  // EXCLUSIVO MODO BÁSQUET: 10 minutos por cuarto y 5 faltas
  sportMode = SPORT_BASKET;
  homeColorIdx   = prefs.getUChar("col_home", 0) % NUM_TEAM_COLORS; // Por defecto: Ámbar
  awayColorIdx   = prefs.getUChar("col_away", 1) % NUM_TEAM_COLORS; // Por defecto: Verde
  periodColorIdx = prefs.getUChar("col_per",  2) % NUM_TEAM_COLORS; // Por defecto: Celeste
  timeColorIdx   = prefs.getUChar("col_time", 1) % NUM_TEAM_COLORS; // Por defecto: Verde puro
  configuredPeriodMinutes = BASKET_PERIOD_MINUTES; // 10 min
  remainingMs = (int32_t)configuredPeriodMinutes * 60 * 1000L;
  prefs.putUChar("sport", (uint8_t)SPORT_BASKET);
  prefs.putUChar("period_min", configuredPeriodMinutes);

  initLedsRMT();
  clearLeds();

  setupEspNow();

  Serial.println("\n=======================================================");
  Serial.printf ("  TABLERO DEPORTIVO - MODO BÁSQUET EXCLUSIVO (10:00)\n");
  Serial.printf ("  Pin de datos LED: GPIO %d (D4 en placa)\n", LED_PIN);
  Serial.printf ("  Total de LEDs:    %d\n", NUM_LEDS);
  Serial.println("  Iniciando directamente en pantalla de juego.");
  Serial.println("=======================================================\n");

  bootSequence();

  // Dibuja de inmediato la pantalla oficial de Básquet en pausa y limpia
  render();

  lastTick = millis();
}

void loop() {
  otaServer.handleClient();

  // --- MODO DE JUEGO NORMAL ---
  if (rxBlinkUntil && (int32_t)(millis() - rxBlinkUntil) >= 0) {
    rxBlinkUntil = 0;
    digitalWrite(STATUS_LED, LOW);
  }

  while (qTail != qHead) {
    CmdPacket p = rxQueue[qTail];
    qTail = (qTail + 1) % RXQ;
    handleCommand(p);
  }

  uint32_t now = millis();
  if (running) {
    remainingMs -= (int32_t)(now - lastTick);
    if (remainingMs <= 0) {
      remainingMs = 0; 
      running = false;
      isDirty = true; // Actualización obligatoria para dibujar 00:00 exacto
      startHorn(HORN_END_MS);
      finishFlashUntil = now + HORN_END_MS + 1000;
    }
  }
  lastTick = now;
  updateHornOutputs();

#if defined(EMERGENCY_CLOCK_ONLY) && EMERGENCY_CLOCK_ONLY
  if (clockRealTimeMode) {
    if (now - lastClockSecTick >= 1000) {
      lastClockSecTick = now;
      clockSecond++;
      if (clockSecond >= 60) {
        clockSecond = 0;
        clockMinute++;
        if (clockMinute >= 60) {
          clockMinute = 0;
          clockHour = (clockHour + 1) % 24;
        }
      }
      isDirty = true;
    }
  }
#endif

  // ================== ZERO-FLICKER & RESPONSIVE RENDER ENGINE ==================
  if (running && solidTestMode == 0) {
    if (remainingMs < 60000) {
      // En el último minuto: refrescar solo cuando cambia la décima de segundo (cada 100 ms)
      static int32_t lastRenderedTenth = -1;
      int32_t currentTenth = remainingMs / 100;
      if (currentTenth != lastRenderedTenth) {
        lastRenderedTenth = currentTenth;
        isDirty = true;
      }
    } else {
      // Minutos normales: refrescar cada 1 segundo (cuando cambia el segundo)
      int32_t currentSec = remainingMs / 1000;
      if (currentSec != lastRenderedSecond) {
        lastRenderedSecond = currentSec;
        isDirty = true;
      }
    }
  }

  // Si estamos en la secuencia de fin de tiempo (parpadeo de 00:00): refrescar cada 250 ms
  if (remainingMs == 0 && (int32_t)(finishFlashUntil - now) > 0 && solidTestMode == 0) {
    static uint32_t lastFlashTick = 0;
    if (now - lastFlashTick >= 250) {
      lastFlashTick = now;
      isDirty = true;
    }
  }

  // Si hubo cambio de puntaje, período, faltas, modo prueba o tic del reloj: renderizar
  if (isDirty) {
    delay(4); // Permite al stack Wi-Fi/ESP-NOW completar el envío del ACK sin colisionar
    render();
  }
}
