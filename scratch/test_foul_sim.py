def sim_fouls(homeFouls, awayFouls, sportMode):
    FOUL_BLOCKS_PER_SIDE = 6
    LEDS_PER_FOUL_BLOCK = 4
    FOUL_HOME_IN_ORDER = False
    FOUL_AWAY_IN_ORDER = True
    OFF_FOUL_AWAY = 106
    OFF_FOUL_HOME = 130
    
    # AWAY
    away_blocks = []
    for b in range(FOUL_BLOCKS_PER_SIDE):
        foulIdx = b if FOUL_AWAY_IN_ORDER else (FOUL_BLOCKS_PER_SIDE - 1 - b)
        off = OFF_FOUL_AWAY + b * LEDS_PER_FOUL_BLOCK
        col = "BLACK"
        if awayFouls > foulIdx:
            if sportMode == "BASKET":
                col = "TURQUESA" if foulIdx < 3 else "ROJO"
            else: # FUTSAL
                col = "TURQUESA" if foulIdx < 5 else "ROJO"
        if sportMode == "BASKET" and foulIdx >= 5:
            col = "BLACK"
        away_blocks.append((foulIdx+1, off, col))
        
    # HOME
    home_blocks = []
    for b in range(FOUL_BLOCKS_PER_SIDE):
        foulIdx = b if FOUL_HOME_IN_ORDER else (FOUL_BLOCKS_PER_SIDE - 1 - b)
        off = OFF_FOUL_HOME + b * LEDS_PER_FOUL_BLOCK
        col = "BLACK"
        if homeFouls > foulIdx:
            if sportMode == "BASKET":
                col = "TURQUESA" if foulIdx < 3 else "ROJO"
            else: # FUTSAL
                col = "TURQUESA" if foulIdx < 5 else "ROJO"
        if sportMode == "BASKET" and foulIdx >= 5:
            col = "BLACK"
        home_blocks.append((foulIdx+1, off, col))
        
    return home_blocks, away_blocks

for fouls in range(0, 7):
    hb, ab = sim_fouls(fouls, fouls, "BASKET")
    print(f"\n--- BASKET CON {fouls} FALTAS ---")
    print("HOME:", [(f"Falta {f}", f"LED {off}..{off+3}", col) for f, off, col in hb if col != "BLACK"])
    print("AWAY:", [(f"Falta {f}", f"LED {off}..{off+3}", col) for f, off, col in ab if col != "BLACK"])

