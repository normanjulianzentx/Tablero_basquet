# Simulación píxel a píxel del Tablero Deportivo
# Verifica rangos exactos de LEDs para cada elemento

BUFFER_LED_COUNT = 1
LEDS_PER_SEG = 3
LEDS_PER_DIGIT = 21 # 7 segmentos * 3
LEDS_PER_COLON_DOT = 1
LEDS_COLON_TOTAL = 2

FOUL_BLOCKS_PER_SIDE = 6
LEDS_PER_FOUL_BLOCK = 4
LEDS_FOULS_PER_SIDE = 24

OFF_HOME = BUFFER_LED_COUNT                             # 1..42
OFF_PERIOD = OFF_HOME + 2 * LEDS_PER_DIGIT              # 43..63
OFF_AWAY = OFF_PERIOD + LEDS_PER_DIGIT                  # 64..105
OFF_FOUL_AWAY = OFF_AWAY + 2 * LEDS_PER_DIGIT          # 106..129
OFF_FOUL_HOME = OFF_FOUL_AWAY + LEDS_FOULS_PER_SIDE    # 130..153
OFF_TIME_D1 = OFF_FOUL_HOME + LEDS_FOULS_PER_SIDE      # 154..174
OFF_TIME_D2 = OFF_TIME_D1 + LEDS_PER_DIGIT             # 175..195
OFF_TIME_COLON = OFF_TIME_D2 + LEDS_PER_DIGIT         # 196..197
OFF_TIME_D3 = OFF_TIME_COLON + LEDS_COLON_TOTAL        # 198..218
OFF_TIME_D4 = OFF_TIME_D3 + LEDS_PER_DIGIT             # 219..239
NUM_LEDS = OFF_TIME_D4 + LEDS_PER_DIGIT                # 240

print("=== MAPA EXACTO DE PÍXELES (TOTAL 240 LEDs: 0 a 239) ===")
print(f"LED   0         : Píxel de sacrificio en PCB (SIEMPRE NEGRO/APAGADO)")
print(f"LED   1 ..  21  : LOCAL Decena (21 LEDs)")
print(f"LED  22 ..  42  : LOCAL Unidad (21 LEDs)")
print(f"LED  43 ..  63  : PERÍODO (21 LEDs)")
print(f"LED  64 ..  84  : VISITA Decena (21 LEDs)")
print(f"LED  85 .. 105  : VISITA Unidad (21 LEDs)")
print(f"LED 106 .. 129  : FALTAS VISITA (6 bloques de 4 = 24 LEDs)")
for b in range(6):
    print(f"       Bloque {b} (Falta {b+1}): LED {OFF_FOUL_AWAY + b*4} .. {OFF_FOUL_AWAY + b*4 + 3}")
print(f"LED 130 .. 153  : FALTAS LOCAL (6 bloques de 4 = 24 LEDs)")
for b in range(6):
    foulNum = 6 - b # En reversa: bloque 0 es falta 6, bloque 5 es falta 1
    print(f"       Bloque {b} (Falta {foulNum}): LED {OFF_FOUL_HOME + b*4} .. {OFF_FOUL_HOME + b*4 + 3}")
print(f"LED 154 .. 174  : TIEMPO D1 - Minutos Decena / Segundos Decena (21 LEDs)")
print(f"LED 175 .. 195  : TIEMPO D2 - Minutos Unidad / Segundos Unidad (21 LEDs)")
print(f"LED 196 .. 197  : TIEMPO Dos Puntos ':' (2 LEDs)")
print(f"LED 198 .. 218  : TIEMPO D3 - Segundos Decena / Décimas (21 LEDs)")
print(f"LED 219 .. 239  : TIEMPO D4 - Segundos Unidad / Centésimas (21 LEDs)")

