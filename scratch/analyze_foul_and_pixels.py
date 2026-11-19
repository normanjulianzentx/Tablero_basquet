import re

with open('simulador_tablero.html', 'r', encoding='utf-8') as f:
    text = f.read()

boxes = re.findall(r'<div class="foul-box.*?>', text)
print(f"Total foul boxes in HTML: {len(boxes)}")
for b in boxes:
    print("  ", b)

with open('include/config.h', 'r', encoding='utf-8') as f:
    cfg = f.read()

print("\n--- config.h offsets ---")
for line in cfg.splitlines():
    if 'OFF_' in line or 'FOUL_' in line or 'NUM_LEDS' in line or 'BUFFER_' in line:
        print("  ", line)

