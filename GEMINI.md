# REGLAS Y ESPECIFICACIÓN TÉCNICA MAESTRA - TABLERO DEPORTIVO BÁSQUET & FUTSAL

Este archivo fija de forma permanente la arquitectura, el hardware real instalado y la configuración del proyecto en `c:\Users\Usuario\Documents\Tablero_basquet` para evitar cualquier olvido o regresión en futuras sesiones.

---

## 1. HARDWARE FÍSICO REAL INSTALADO (CONTROLADOR PCB 5x5 cm)
* **Microcontrolador**: ESP32-C3 SuperMini V1 IPEX (160 MHz RISC-V, 4MB Flash).
  * MAC Tablero: `AC:27:6E:FD:34:D0`
  * MAC Control Remoto (ZenTX): `AC:27:6E:FD:3D:00`
* **Etapa de Datos (Level Shifter sin integrados)**:
  * **NO se usa SN74LS07N** (inconseguible localmente).
  * **Solución real**: Diodo **1N4007** en serie con la alimentación del primer LED de la tira montado en placa (**LED 0 de sacrificio / Buffer WS2811**).
  * El diodo baja los 5.0V a ~4.3V-4.4V en el LED 0, permitiendo que la señal de datos de 3.3V del GPIO 4 del ESP32 sea leída al 100% como un nivel lógico alto ($V_{IH}$).
  * La salida `DO` del LED 0 regenera los pulsos a 5.0V cuadrados limpios hacia el tablero.
  * Resistencia **R2 de 330 Ω** en serie en la línea de datos hacia la bornera de salida.
* **Filtros de Alimentación (Obligatorios)**:
  * Capacitor electrolítico **C1 = 1000 µF / 16V o 25V** pegado a la bornera de entrada de 5V (elimina caídas de tensión bruscas y píxeles rojos parásitos).
  * Capacitor cerámico **C2 = 100 nF (104)** pegado al ESP32.
* **Periféricos en Placa**:
  * **GPIO 4**: Salida de datos hacia el LED de sacrificio.
  * **GPIO 5**: Salida hacia módulo de relé optoacoplado de 5V (Sirena 12V de cancha, activa en LOW).
  * **GPIO 6**: Piezoeléctrico pasivo (Buzzer integrado para pitidos de confirmación).
  * **GPIO 8**: LED azul de estado onboard.

---

## 2. TOPOLOGÍA DE LA TIRA SERIE WS2811 (239 LEDs TOTAL)
* **Tipo de LED**: Tira direccionable WS2811 (5V, 800 kHz).
* **Orden de color FÍSICO verificado**: **`RGB`** (Red-Green-Blue).
* **Brillo configurado**: `LED_BRIGHTNESS 140` (evita caídas de tensión inductivas).
* **Consumo de seguridad**: `MAX_MILLIAMPS 7500` (7.5A).
* **Driver de Transmisión WS2811 en ESP32-C3 (CRÍTICO - NO REEMPLAZAR POR `FastLED.show()`)**:
  * **Bug en FastLED 3.6.0 sobre ESP32-C3**: Al finalizar cada trama, `clockless_rmt_esp32.cpp` ejecuta `gpio_matrix_out(LED_PIN, 0x100, 0, 0)`. En el ESP32-C3 el registro `func_sel` es de 8 bits (`0..255`), por lo que `0x100` (`256`) se trunca a `0x00` (`SPIQ_OUT_IDX`, bus de la memoria Flash SPI interna), inyectando ruido de la Flash en `GPIO 4`, corrompiendo la decena de Local, corriendo 1 píxel las faltas y dejando en rojo el último LED.
  * **Solución definitiva**: Driver nativo RMT de ESP-IDF en `src/board/main.cpp` (`initLedsRMT()` + `showLeds()`), que precalcula los 240 LEDs en RAM (`rmtItems[240 * 24 + 1]`), mantiene `GPIO 4` permanentemente conectado al periférico RMT en `RMT_IDLE_LEVEL_LOW` (`0.0V`) y envía un pulso final de Reset de 300 µs en LOW.

### Mapeo Píxel a Píxel (Con LED 0 buffer de sacrificio en PCB, 240 LEDs total):
* `LED 0`: **Buffer de sacrificio en PCB** (siempre apagado en software).
* `LED 1 .. 21` (21 LEDs): **LOCAL Decena** (Segmentos A..G, 3 LEDs por segmento).
* `LED 22 .. 42` (21 LEDs): **LOCAL Unidad**.
* `LED 43 .. 63` (21 LEDs): **PERÍODO** (Dígitos 1..9).
* `LED 64 .. 84` (21 LEDs): **VISITA Decena**.
* `LED 85 .. 105` (21 LEDs): **VISITA Unidad**.
* `LED 106 .. 129` (24 LEDs): **FALTAS VISITA** (6 bloques de 4 LEDs; Bloque 5 en 126..129 centro apagado; Bloques 0..4 de afuera hacia el centro).
* `LED 130 .. 153` (24 LEDs): **FALTAS LOCAL** (6 bloques de 4 LEDs; Bloque 0 en 130..133 centro apagado; Bloques 5..1 de afuera hacia el centro).
* `LED 154 .. 174` (21 LEDs): **TIEMPO D1** (Minutos Decena, o Segundos Decena en el último minuto).
* `LED 175 .. 195` (21 LEDs): **TIEMPO D2** (Minutos Unidad, o Segundos Unidad en el último minuto).
* `LED 196 .. 197` (2 LEDs): **TIEMPO Dos Puntos `:`** (En Verde deportivo permanente).
* `LED 198 .. 218` (21 LEDs): **TIEMPO D3** (Segundos Decena, o Décimas en el último minuto).
* `LED 219 .. 239` (21 LEDs): **TIEMPO D4** (Segundos Unidad, o Centésimas en el último minuto).

---

## 3. REGLAS DEPORTIVAS Y VISUALIZACIÓN (MODO BÁSQUET EXCLUSIVO)
* **Deporte Fijado**: **BÁSQUETBOL FIBA ÚNICAMENTE** (Modo Futsal desactivado en firmware para máxima estabilidad).
  * Tiempo por defecto actual: **01:00 minuto** (seleccionable en vivo entre **1, 10, 12 y 20 minutos** para cuartos, y **1, 2, 5 y 15 minutos** para Entretiempo).
  * **Modo Entretiempo (`E`)**: Al activar cualquier descanso/entretiempo (desde la web `E - 1 min`, `E - 2 min`, `E - 5 min`, `E - 15 min` o manteniendo presionado el botón físico **Período** por 1.5s sin Shift), el dígito central de **PERÍODO** dibuja la letra **`E`** (`0b1111001`, segmentos A, D, E, F, G). Al avanzar de período o elegir un tiempo de cuarto normal, vuelve a mostrar el número de período (`1..9`).
  * Al encender: Inicia directamente en **pantalla oficial de partido lista y en pausa** (`01:00` o el último tiempo guardado en NVS, `00` - `00`, Período `1`, faltas apagadas).
  * **Sin secuencias ni animaciones de espera**: No realiza barridos de colores ni arcoíris al encender con el control apagado para evitar ruido o distracción.
* **Colores Oficiales (Configurables en vivo desde `T-B-O-T-A` y desde el Control Remoto `ZenTX`, guardados en memoria NVS)**:
  * **LOCAL** (`col_home`): Por defecto Ámbar puro `CRGB(255, 120, 0)` (7 colores elegibles; también ciclable manteniendo presionado el botón físico **LOCAL** por 1.5s).
  * **VISITA** (`col_away`): Por defecto Verde deportivo puro `CRGB(0, 220, 40)` (7 colores elegibles; también ciclable manteniendo presionado el botón físico **VISITA** por 1.5s).
  * **PERÍODO / E** (`col_per`): Por defecto Celeste / Cian `CRGB(0, 160, 255)` (7 colores elegibles desde la web del control o del tablero).
  * **TIEMPO** (`col_time`): Por defecto Verde deportivo puro `CRGB(0, 255, 0)` (7 colores elegibles desde la web del control o del tablero).
* **Tanteadores (Local y Visita)**:
  * Formato de 2 dígitos completos (`00` a `99`).
  * Las decenas **NUNCA se apagan** con tanteadores menores a 10 (con 0 puntos muestra `00`, con 1 punto `01`).
* **Reloj de Tiempo**:
  * Formato normal ($t \ge 60\text{ s}$): `MM:SS`.
  * **Último minuto** ($t < 60\text{ s}$): Cambia automáticamente a **Segundos y Décimas/Centésimas** (`SS:dc`) refrescando fluidamente a 50 ms.
  * Al llegar a cero: Dibuja exactamente `00:00`, hace sonar la sirena/relé y parpadea en rojo la secuencia de fin de cuarto.
* **Faltas Colectivas**:
  * Límite estricto: **5 faltas colectivas**.
  * Faltas 1, 2 y 3: **VERDE PURO** `COLOR_FOUL_NORMAL` (`CRGB(0, 255, 0)`).
  * Faltas 4 y 5: **ROJO VIVO** `COLOR_FOUL_PENALTY` (`CRGB(255, 0, 0)`, penalidad / bonus de tiros libres).
  * **6to Bloque (4 LEDs centrales por equipo)**:
    * Visita Bloque 5 (LEDs 126..129, centro): **DIRECTAMENTE APAGADOS** (`CRGB::Black`).
    * Local Bloque 0 (LEDs 130..133, centro): **DIRECTAMENTE APAGADOS** (`CRGB::Black`).
    * Estos 8 LEDs del centro (126..133) permanecen permanentemente apagados para mantener la posición serie exacta de 24 LEDs por lado y limitar el modo Básquet a 5 faltas.
* **Reinicio Total de Partido a Cero (`00 - 00`, Período `1`, Faltas `0 - 0`, Tiempo en pausa)**:
  * **Desde los botones físicos del Control Remoto (`ZenTX`)**:
    1. **Shift + Bocina** (toque directo en cualquier momento).
    2. **Shift + Período** estando en Período `1`, o **doble toque rápido (< 2.5s) de Shift + Período** desde cualquier período.
    3. Mantener presionado **Shift + Período** durante 1.5 segundos.
  * **Desde el celular en `T-B-O-T-A` (`http://192.168.4.1`)**: Botón rojo **"🔄 NUEVO PARTIDO: RESETEAR TODO A CERO (00-00)"**.
  * **Desde el celular en `ZenTX` (`http://192.168.4.1`)**: Botón rojo **"🔄 REINICIAR PARTIDO A CERO (0-0)"**.
  * Emite doble pitido de confirmación y deja todo listo para el siguiente partido sin apagar el tablero.

---

## 4. SISTEMA DE ACTUALIZACIÓN REMOTA (WEB OTA)
* **SoftAP Wi-Fi Transmitido por el Tablero**:
  * **SSID**: `T-B-O-T-A`
  * **Password**: `12345678`
  * **IP Web Server**: `http://192.168.4.1/update`
* **Binarios disponibles**:
  * `firmware_tablero.bin`: Versión actual completa con personalización de color de **Local, Visita, Período y Tiempo**, faltas 1-2-3 en Verde / 4-5 en Rojo, panel de prueba RGBW y driver RMT nativo sin interferencias.
  * `firmware_tablero_ESTABLE_v1.bin`: Respaldo de seguridad de la versión estable verificada en cancha.
  * `firmware_tablero_EMERGENCIA_RELOJ.bin`: **Modo Emergencia Reloj Puro** (Apaga completamente Local, Visita, Período y Faltas; muestra ÚNICAMENTE el tiempo en Verde deportivo; Start/Stop desde control remoto y desde celular; bocina automática al finalizar; opción de sincronizar hora real del teléfono).
* Permite calibrar y actualizar cualquier ajuste de firmware desde un celular o PC conectada al Wi-Fi del tablero sin desarmarlo ni conectarle cables.
