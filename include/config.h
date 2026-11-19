#pragma once
#include <stdint.h>
#include <FastLED.h>

// ================== PINES ==================
#if defined(CONFIG_IDF_TARGET_ESP32C3)
  // ESP32-C3 Mini / SuperMini V1 IPEX
  #define LED_PIN          4        // Datos WS2811 en GPIO 4 (vía SN74LS07N + pull-up 1k a 5V + 330 ohm)
  #define RELAY_PIN        5        // Relé para sirena externa 12V en GPIO 5
  #define BUZZER_PIN       6        // Piezoeléctrico pasivo en GPIO 6
  #define STATUS_LED       8        // LED azul onboard en GPIO 8 (ESP32-C3 SuperMini)
  #define PIN_BOARD_TEST   255      // Desactivado en C3 para evitar disparos por pin flotante
#else
  // ESP32 DevKit v1 clásico (30 pines)
  #define LED_PIN          4        // Datos WS2811 en GPIO 4 / D4
  #define RELAY_PIN        25       // Relé para sirena externa 12V en GPIO 25
  #define BUZZER_PIN       27       // Piezoeléctrico pasivo en GPIO 27
  #define STATUS_LED       2        // LED azul onboard en GPIO 2
  #define PIN_BOARD_TEST   13       // Botón de prueba / tracer en GPIO 13 (D13 a GND con pull-up)
#endif

// Orden de color de los chips WS2811 (Físico estándar en tiras WS2811: RGB)
#define LED_COLOR_ORDER        RGB

#define RELAY_ACTIVE_LOW  true    // Módulos de relé estándar activan con LOW
#define BUZZER_FREQ_HZ    2700    // Frecuencia típica del piezo
#define BUZZER_LEDC_CH    0

// ================== MODOS DEPORTIVOS (BÁSQUET / FUTSAL) ==================
enum SportMode : uint8_t {
  SPORT_BASKET = 0,
  SPORT_FUTSAL = 1
};

#define DEFAULT_SPORT_MODE     SPORT_BASKET   // Modo de inicio por defecto (Básquet)

// Reglas de tiempo y faltas por deporte
#define BASKET_PERIOD_MINUTES  1              // Por defecto 1:00 min (configurable a 1, 2, 5, 10, 12, 15, 20 min)
#define FUTSAL_PERIOD_MINUTES  20             // Tiempos de 20:00 (estándar FIFA / AMF)
#define BASKET_MAX_FOULS       5              // Faltas colectivas: 1..4 ámbar, 5ta ROJA (penalidad/bonus)
#define FUTSAL_MAX_FOULS       6              // Faltas colectivas: 1..5 ámbar, 6ta ROJA (tiro libre sin barrera)

// ================== CONFIGURACIÓN DE LEDs ==================
// Tablero Línea Simple (1.00 x 0.80 m)
#define LED_BRIGHTNESS         140      // 140 = Brillo óptimo deportivo, evita caídas de tensión y píxeles rojos espurios
#define MAX_MILLIAMPS          7500     // 7.5A de tope de seguridad para fuente (según ingeniería eléctrica)

// --- Dígitos de 7 segmentos (Línea simple) ---
#define LEDS_PER_SEG           3        // 3 LEDs por segmento
#define LEDS_PER_DIGIT         (7 * LEDS_PER_SEG)         // 21 LEDs por dígito (7x3)
#define LEDS_PER_COLON_DOT     1                          // 1 LED por punto en los dos puntos ':'
#define LEDS_COLON_TOTAL       (2 * LEDS_PER_COLON_DOT)   // 2 LEDs total en ':'

// --- Cuadrados de Faltas (6 por equipo) ---
#define FOUL_BLOCKS_PER_SIDE   6        // 6 cuadrados para Local, 6 cuadrados para Visita
#define LEDS_PER_FOUL_BLOCK    4        // 4 LEDs por cuadrado (grilla 2x2)
#define LEDS_FOULS_PER_SIDE    (FOUL_BLOCKS_PER_SIDE * LEDS_PER_FOUL_BLOCK)  // 24 LEDs por lado
#define LEDS_FOULS_TOTAL       (2 * LEDS_FOULS_PER_SIDE)                      // 48 LEDs total

// ================== ORDEN DE LA CADENA SERIE WS2811 ==================
// Con 1 LED buffer de sacrificio en placa + diodo 1N4007 (LED 0 en PCB, siempre apagado):
#define BUFFER_LED_COUNT       1

#define OFF_HOME               BUFFER_LED_COUNT                                    //   1..42  (42 LEDs - Local Decena y Unidad)
#define OFF_PERIOD             (OFF_HOME + 2 * LEDS_PER_DIGIT)                     //  43..63  (21 LEDs - Período)
#define OFF_AWAY               (OFF_PERIOD + LEDS_PER_DIGIT)                       //  64..105 (42 LEDs - Visita Decena y Unidad)
#define OFF_FOUL_AWAY          (OFF_AWAY + 2 * LEDS_PER_DIGIT)                     // 106..129 (24 LEDs - Faltas Visita)
#define OFF_FOUL_HOME          (OFF_FOUL_AWAY + LEDS_FOULS_PER_SIDE)               // 130..153 (24 LEDs - Faltas Local)
#define OFF_TIME_D1            (OFF_FOUL_HOME + LEDS_FOULS_PER_SIDE)               // 154..174 (21 LEDs - Minutos Decena)
#define OFF_TIME_D2            (OFF_TIME_D1 + LEDS_PER_DIGIT)                      // 175..195 (21 LEDs - Minutos Unidad)
#define OFF_TIME_COLON         (OFF_TIME_D2 + LEDS_PER_DIGIT)                      // 196..197 (  2 LEDs - Dos puntos ':')
#define OFF_TIME_D3            (OFF_TIME_COLON + LEDS_COLON_TOTAL)                 // 198..218 (21 LEDs - Segundos Decena)
#define OFF_TIME_D4            (OFF_TIME_D3 + LEDS_PER_DIGIT)                      // 219..239 (21 LEDs - Segundos Unidad)

#define NUM_LEDS               (OFF_TIME_D4 + LEDS_PER_DIGIT)                      // 240 LEDs en total exactos (1 buffer + 239 en tablero)

// Dirección de los bloques de faltas en la tira serie:
// - Faltas Visita: entra por el bloque exterior derecho (Falta 1) y avanza al centro (Falta 6, roja).
#define FOUL_AWAY_IN_ORDER     true    // true = Bloques 0..5 en la tira son Faltas 1..6
// - Faltas Local: entra por el centro (Falta 6, roja) y avanza al extremo izquierdo (Falta 1).
#define FOUL_HOME_IN_ORDER     false   // false = Bloque 0 es Falta 6 (centro), Bloque 5 es Falta 1 (izq)

// Orden físico de segmentos dentro de un dígito (posición en la tira -> segmento lógico).
// Lógicos: 0=A(arriba) 1=B(arr.der) 2=C(baj.der) 3=D(abajo) 4=E(baj.izq) 5=F(arr.izq) 6=G(medio)
static const uint8_t SEG_WIRING[7] = {0, 1, 2, 3, 4, 5, 6};

// ================== COLORES DE JUEGO (idénticos al diseño gráfico) ==================
#define COLOR_LOCAL            CRGB(255, 120, 0)    // Ámbar / Naranja dorado para LOCAL
#define COLOR_AWAY             CRGB(0, 220, 40)     // Verde deportivo para VISITA
#define COLOR_PERIOD           CRGB(0, 160, 255)    // Celeste / Cyan para PERIODO
#define COLOR_TIME             CRGB(0, 255, 0)      // Verde deportivo puro para TIEMPO
#define COLOR_TIME_LOW         CRGB(0, 255, 0)      // Verde deportivo constante
#define COLOR_FOUL_NORMAL      CRGB(0, 255, 0)      // Verde puro para faltas 1, 2 y 3
#define COLOR_FOUL_PENALTY     CRGB(255, 0, 0)      // Rojo vivo para faltas 4 y 5 (penalidad)

// ================== JUEGO Y TIEMPO ==================
#define MAX_PERIOD             9
#define HORN_END_MS            2500     // Bocina automática al llegar a 00:00
#define HORN_MAX_MS            5000     // Tope de seguridad bocina

// ================== SECUENCIA DE ARRANQUE Y PRUEBA ==================
#define BOOT_TEST_DIGITS       false    // Desactivado: inicia directo en pantalla de juego lista (10:00, 00-00, P1)
#define BOOT_TEST_DELAY_MS     60       // Velocidad de cuenta 0..8 en cada dígito (ms)
#define BOOT_COLOR_HOLD_MS     700      // Tiempo por color en la prueba RGB (ms)
#define BOOT_FINAL_HOLD_MS     1800     // Tiempo con colores de juego encendidos al final (ms)
#define BOOT_BEEP              false    // Sin pitido molesto al encender en la cancha
