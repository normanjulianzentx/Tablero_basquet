# Tablero Deportivo Multidisciplina – Básquet & Futsal (ESP32 + WS2811 + ESP-NOW)

Tablero deportivo electrónico de **239 LEDs WS2811 (5V)** en serie desde un único pin de datos (GPIO 4 / D4).
Control inalámbrico de ultra baja latencia con otro ESP32 vía **ESP-NOW cifrado** (con confirmación de recepción ACK y anti-replay).

---

## 🏀 Modos de Juego: Básquet y Futsal

El tablero se adapta a ambos deportes conservando la configuración en memoria permanente (NVS Flash):

| Característica | 🏀 Básquetbol | ⚽ Futsal |
|---|---|---|
| **Duración del período** | 10:00 min (4 cuartos FIBA) | 20:00 min (2 tiempos FIFA/AMF) |
| **Faltas acumulables (Ámbar)** | Cuadrados 1 a 4 | Cuadrados 1 a 5 |
| **Falta de penalidad (Rojo)** | **5ta falta** (Bonus de tiros libres) | **6ta falta** (Tiro libre 10m sin barrera) |
| **Cuadrados restantes** | El 6to cuadrado permanece apagado | Los 6 cuadrados operativos |
| **Aviso acústico de penalidad** | Pitido largo al alcanzar la 5ta falta | Pitido largo al alcanzar la 6ta falta |
| **Cambio de modo** | Tecla **Retroceder (GPIO 18) + Período (GPIO 33)** por 1.5 segundos | Tecla **Retroceder + Período** por 1.5 segundos |

> Al cambiar de deporte, el dígito central de período muestra momentáneamente **'b'** (Básquet) o **'F'** (Futsal) y emite un pitido de confirmación.

---

## 💡 Cadena de LEDs (239 LEDs WS2811)

Línea simple de 3 LEDs por segmento (`LEDS_PER_SEG = 3` → 21 LEDs por dígito).

```
[ LOCAL ]        [ PERIODO ]        [ VISITA ]
  (2 dig)          (1 dig)           (2 dig)
  42 LEDs          21 LEDs           42 LEDs
   Ámbar           Celeste            Verde

  [◽◽◽◽◽🔴]                     [🔴◽◽◽◽◽]
Faltas Local (24 LEDs)            Faltas Visita (24 LEDs)
  1..5 ámbar, 6ta roja              1..5 ámbar, 6ta roja

                 [  TIEMPO  ]
             4 dígitos + ':' (86 LEDs)
             Blanco (Rojo en último min)
```

### Tabla de rangos y offsets (ajustable en `include/config.h`)
| Zona | Cantidad | LEDs por elemento | Total LEDs | Rango en tira | Color oficial |
|---|---|---|---|---|---|
| **Local** | 2 dígitos | 21 LEDs / díg. (7 seg × 3) | 42 | 0 – 41 | Ámbar (`255, 130, 0`) |
| **Período** | 1 dígito | 21 LEDs / díg. (7 seg × 3) | 21 | 42 – 62 | Celeste (`0, 160, 255`) |
| **Visita** | 2 dígitos | 21 LEDs / díg. (7 seg × 3) | 42 | 63 – 104 | Verde (`0, 220, 40`) |
| **Faltas Local** | 6 cuadrados | 4 LEDs / cuadrado (2×2) | 24 | 105 – 128 | 1..5 Ámbar, 6ta Roja |
| **Faltas Visita** | 6 cuadrados | 4 LEDs / cuadrado (2×2) | 24 | 129 – 152 | 1..5 Ámbar, 6ta Roja (simétrico) |
| **Tiempo (Minutos)** | 2 dígitos | 21 LEDs / díg. | 42 | 153 – 194 | Blanco (`CRGB::White`) |
| **Dos Puntos (':')** | 2 puntos | 1 LED / punto | 2 | 195 – 196 | Blanco pulsante |
| **Tiempo (Segundos)** | 2 dígitos | 21 LEDs / díg. | 42 | 197 – 238 | Blanco (`CRGB::White`) |
| **TOTAL TABLERO** | | | **239 LEDs** | **0 – 238** | |

---

## 🚀 Secuencia de Arranque y Modo de Prueba de LEDs

Al encender el tablero o al solicitarlo desde el control remoto:
1. **Conteo en cascada (0 al 8)**: Recorre cada uno de los 9 dígitos numéricos dibujando del 0 al 8 progresivamente.
2. **Encendido secuencial de faltas**: Enciende los 12 cuadrados de faltas (ámbar y rojo) y los dos puntos `:` del reloj.
3. **Ciclo de prueba de colores (con 100% de los LEDs en '8')**:
   - 🔴 **Rojo puro**: Verifica los chips rojos de cada pixel.
   - 🟢 **Verde puro**: Verifica los chips verdes.
   - 🔵 **Azul puro**: Verifica los chips azules.
   - 🟡 **Ámbar / Amarillo**: Verifica la mezcla de canales R + G.
   - ⚪ **Blanco máximo (100% potencia)**: Prueba de carga eléctrica y estabilidad de la fuente a corriente máxima.
   - 🏆 **Colores de juego**: Muestra la distribución deportiva definitiva (Local en ámbar, Visita en verde, Período en celeste, Faltas en ámbar/rojo y Tiempo en blanco).
4. **Pitido y atenuación suave**: Señal acústica con el buzzer y fade-out progresivo.
5. **Interrupción inmediata**: Si en cualquier momento el control remoto envía una orden, la prueba se cancela al instante para no demorar el inicio del partido.

---

## 🎮 Controles del Remoto (ESP32)

Todos los pulsadores van conectados entre el pin indicado y **GND** (con `INPUT_PULLUP` interno).
El pulsador en **GPIO 18** actúa como tecla modificadora maestra (**Shift / Retroceder**).

| GPIO | Función Normal (Toque) | Función con Retroceder (Shift) | Función Mantener (>1.5s) |
|---|---|---|---|
| **13** | Tiempo Start / Stop | Tiempo Start / Stop | **Reset de tiempo** (10:00 o 20:00) |
| **14** | Gol / Puntos Local (+1) | Restar Local (-1) | — |
| **27** | Gol / Puntos Visita (+1) | Restar Visita (-1) | — |
| **33** | Período (+1) | Retroceder Período (-1) | **Cambiar deporte** (Básquet ↔ Futsal) |
| **26** | Falta Local (+1) | Restar Falta Local (-1) | — |
| **25** | Falta Visita (+1) | Restar Falta Visita (-1) | — |
| **4**  | Bocina manual (1.5 seg) | **Test de dígitos y colores** | — |
| **18** | — | — | **Tecla Modificadora (Shift)** |

---

## 🛠️ Conexiones del Hardware

- **Placa**: ESP32 DevKit v1 (30 pines) alimentado por pin VIN (5V).
- **Salida de datos LED**: GPIO 4 / D4 conectado al buffer adaptador de nivel **SN74LS07N** (o 74HCT125) con resistencia pull-up de 1 kΩ a +5V y resistencia de salida de 330 Ω al cable DIN de la tira.
- **Relé de sirena 12V**: GPIO 25.
- **Buzzer piezoeléctrico**: GPIO 27.
- **Fuente**: 5V (40A) con alimentación e inyecciones directas en la tira de LEDs y GND unificado con el ESP32.

---

## 💻 Compilación y Carga (PlatformIO)

```bash
# Grabar firmware en el Tablero:
pio run -e board -t upload

# Grabar firmware en el Control Remoto:
pio run -e remote -t upload
```
