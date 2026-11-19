# Simulación del reloj con cambio a décimas en el último minuto

def simulate_clock(remainingMs):
    ms = max(0, remainingMs)
    low = (ms < 60000) # Último minuto (< 60 seg)
    
    if not low:
        # MM:SS (Minutos y Segundos)
        s = ms // 1000
        d1 = (s // 60) // 10
        d2 = (s // 60) % 10
        d3 = (s % 60) // 10
        d4 = (s % 60) % 10
        mode_str = "MM:SS"
    else:
        # SS:cc (Segundos y Décimas/Centésimas)
        ss = ms // 1000
        cc = (ms % 1000) // 10 # 0..99 centésimas
        d1 = ss // 10
        d2 = ss % 10
        d3 = cc // 10          # Décimas (0..9)
        d4 = cc % 10           # Centésimas (0..9)
        mode_str = "SS:dc (Último minuto)"
        
    return f"{remainingMs:6d} ms | Modo {mode_str:22s} | D1={d1} D2={d2} : D3={d3} D4={d4} | Pantalla: {d1}{d2}:{d3}{d4}"

test_times = [
    65000, 61000, 60000, 59990, 59000, 45230, 10500, 5100, 1500, 1000, 950, 500, 100, 50, 0
]

for t in test_times:
    print(simulate_clock(t))

