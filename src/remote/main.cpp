#include "protocol.h"
#include "secrets.h"
#include <Arduino.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <Update.h>
#include <WebServer.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>

// =====================================================================
// ESTRUCTURA Y PINES DE BOTONES FÍSICOS
// =====================================================================
struct Button {
  uint8_t pin;
  Command cmdNormal;
  Command cmdBack;
  int16_t valueNormal;
  int16_t valueBack;
  bool lastState; // true = suelto (pull-up)
  uint32_t lastChange;
  uint32_t pressedAt;
  bool shiftOnPress;
  bool longFired;
};

#if defined(CONFIG_IDF_TARGET_ESP32C3)
#define STATUS_LED 8 // LED azul onboard en GPIO 8 (ESP32-C3 SuperMini)
#define PIN_BTN_BACK                                                           \
  7 // Tecla modificadora maestra: RETROCEDER (SHIFT) en GPIO 7
#define PIN_BTN_TIMER 0 // Tiempo Start/Stop
#define PIN_BTN_HOME 1  // Local
#define PIN_BTN_AWAY 2  // Visita
#define PIN_BTN_PER 3   // Período

// Botones a GND (INPUT_PULLUP) mapeados a pines del ESP32-C3
Button buttons[] = {
    {0, CMD_TIMER_TOGGLE, CMD_TIME_ADD, 0, -30, true, 0,
     0}, // Tiempo Start/Stop (con Shift: -30 segundos)
    {1, CMD_HOME_INC, CMD_HOME_DEC, 0, 0, true, 0,
     0}, // Local (+ / con Shift -)
    {2, CMD_AWAY_INC, CMD_AWAY_DEC, 0, 0, true, 0,
     0}, // Visita (+ / con Shift -)
    {3, CMD_PERIOD_INC, CMD_PERIOD_DEC, 0, 0, true, 0,
     0}, // Período (+ / con Shift -)
    {4, CMD_FOUL_HOME_INC, CMD_FOUL_HOME_DEC, 0, 0, true, 0,
     0}, // Falta Local (+ / con Shift -)
    {5, CMD_FOUL_AWAY_INC, CMD_FOUL_AWAY_DEC, 0, 0, true, 0,
     0}, // Falta Visita (+ / con Shift -)
    {6, CMD_HORN, CMD_GAME_RESET, 1500, 0, true, 0,
     0}, // Bocina (1.5s) / con Shift: Reset Partido 0-0
};
#else
#define STATUS_LED 2
#define PIN_BTN_BACK 18
#define PIN_BTN_TIMER 13
#define PIN_BTN_HOME 14
#define PIN_BTN_AWAY 27
#define PIN_BTN_PER 33

Button buttons[] = {
    {13, CMD_TIMER_TOGGLE, CMD_TIME_ADD, 0, -30, true, 0,
     0}, // Tiempo Start/Stop (con Shift: -30 segundos)
    {14, CMD_HOME_INC, CMD_HOME_DEC, 0, 0, true, 0, 0},
    {27, CMD_AWAY_INC, CMD_AWAY_DEC, 0, 0, true, 0, 0},
    {33, CMD_PERIOD_INC, CMD_PERIOD_DEC, 0, 0, true, 0, 0},
    {26, CMD_FOUL_HOME_INC, CMD_FOUL_HOME_DEC, 0, 0, true, 0, 0},
    {25, CMD_FOUL_AWAY_INC, CMD_FOUL_AWAY_DEC, 0, 0, true, 0, 0},
    {4, CMD_HORN, CMD_TEST_MODE, 1500, 0, true, 0, 0},
};
#endif

const size_t NUM_BUTTONS = sizeof(buttons) / sizeof(buttons[0]);

// =====================================================================
// ESTADO Y COMUNICACIÓN ESP-NOW
// =====================================================================
Preferences prefs;
uint32_t session = 0;
uint32_t seqCounter = 0;

volatile bool ackReceived = false;
volatile uint32_t ackSessionRx = 0, ackSeqRx = 0;
StatePacket lastState = {};

void blink(uint8_t n) {
  for (uint8_t i = 0; i < n; i++) {
    digitalWrite(STATUS_LED, HIGH);
    delay(50);
    digitalWrite(STATUS_LED, LOW);
    delay(60);
  }
}

void onReceive(const uint8_t *mac, const uint8_t *data, int len) {
  if (memcmp(mac, BOARD_MAC, 6) != 0)
    return;
  StatePacket s;
  if (!parsePacket(data, len, PKT_STATE, s))
    return;
  lastState = s;
  ackSessionRx = s.ackSession;
  ackSeqRx = s.ackSeq;
  ackReceived = true;
}

// Envía y espera el ACK del tablero (con reintentos y backoff)
bool sendCommand(Command cmd, int16_t value) {
  CmdPacket p = {};
  p.magic = PROTOCOL_MAGIC;
  p.version = PROTOCOL_VERSION;
  p.type = PKT_COMMAND;
  p.cmd = cmd;
  p.value = value;
  p.session = session;
  p.seq = ++seqCounter;
  sealPacket(p);

  for (uint8_t attempt = 0; attempt < 6; attempt++) {
    ackReceived = false;
    esp_now_send(BOARD_MAC, (uint8_t *)&p, sizeof(p));
    uint32_t t0 = millis();
    while (millis() - t0 < 40) {
      if (ackReceived && ackSessionRx == p.session && ackSeqRx == p.seq) {
        return true;
      }
      delay(1);
    }
  }
  return false;
}

// =====================================================================
// SERVIDOR WEB Y CAPTIVE PORTAL PARA CELULAR
// =====================================================================
const byte DNS_PORT = 53;
DNSServer dnsServer;
WebServer server(80);

// Página web responsive para smartphone (embebida en PROGMEM)
const char PAGE_INDEX[] PROGMEM = R"rawliteral(<!DOCTYPE html>
<html lang="es">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1, user-scalable=no">
<title>Tablero Deportivo</title>
<style>
  * { box-sizing: border-box; -webkit-tap-highlight-color: transparent; }
  body {
    margin: 0; padding: 12px; font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, Helvetica, Arial, sans-serif;
    background: #090d16; color: #f8fafc; text-align: center;
  }
  h1 { font-size: 1.15rem; margin: 4px 0 10px; color: #94a3b8; font-weight: 600; }
  .badge {
    display: inline-block; padding: 4px 10px; font-size: 0.75rem; font-weight: bold;
    border-radius: 999px; text-transform: uppercase; margin-bottom: 8px;
  }
  .badge.ok { background: #065f46; color: #34d399; }
  .badge.warn { background: #854d0e; color: #fde047; }
  
  /* CRONOMETRO */
  .clock-card {
    background: #131b2e; border: 2px solid #1e293b; border-radius: 14px;
    padding: 12px 6px; margin-bottom: 12px; box-shadow: 0 4px 12px rgba(0,0,0,0.5);
  }
  .clock-display {
    font-family: monospace; font-size: 3.2rem; font-weight: 800; letter-spacing: 2px;
    color: #f8fafc; text-shadow: 0 0 16px rgba(56, 189, 248, 0.4); margin: 2px 0;
  }
  .clock-display.running { color: #38bdf8; }
  .period-badge {
    background: #0284c7; color: #fff; font-size: 0.85rem; font-weight: bold;
    padding: 3px 12px; border-radius: 6px; display: inline-block; margin-bottom: 6px;
  }
  .btn-timer {
    width: 96%; max-width: 320px; height: 52px; font-size: 1.15rem; font-weight: 700;
    border: none; border-radius: 10px; cursor: pointer; margin-top: 6px;
    transition: transform 0.1s, opacity 0.2s; touch-action: manipulation;
  }
  .btn-timer.start { background: #16a34a; color: #fff; }
  .btn-timer.pause { background: #ea580c; color: #fff; }
  .btn-timer:active { transform: scale(0.97); }

  /* MARCADOR LOCAL Y VISITA */
  .teams-grid { display: flex; gap: 8px; margin-bottom: 12px; }
  .team-card {
    flex: 1; background: #131b2e; border: 2px solid #1e293b; border-radius: 14px;
    padding: 10px 6px;
  }
  .team-title { font-size: 0.9rem; font-weight: 700; text-transform: uppercase; margin-bottom: 4px; }
  .team-card.local .team-title { color: #fbbf24; }
  .team-card.visita .team-title { color: #4ade80; }
  .score-num { font-size: 3.0rem; font-weight: 900; line-height: 1; margin: 4px 0; font-family: monospace; }
  .team-card.local .score-num { color: #f59e0b; }
  .team-card.visita .score-num { color: #22c55e; }
  .fouls-row { font-size: 0.8rem; color: #cbd5e1; margin-bottom: 8px; }
  .fouls-row b { color: #f87171; font-size: 0.95rem; }

  /* BOTONES DE PUNTOS */
  .btn-row { display: flex; gap: 4px; justify-content: center; margin-bottom: 6px; }
  button {
    font-family: inherit; font-weight: 700; border: none; border-radius: 8px;
    cursor: pointer; touch-action: manipulation;
  }
  button:active { transform: scale(0.95); }
  .btn-pt { flex: 1; height: 42px; font-size: 0.95rem; background: #1e293b; color: #f8fafc; }
  .btn-pt.main { background: #2563eb; color: #fff; font-size: 1.05rem; }
  .btn-pt.sub { background: #334155; color: #94a3b8; }
  .btn-foul { height: 34px; font-size: 0.78rem; background: #3f1515; color: #fca5a5; flex: 1; }

  /* SECCION CONFIGURACION */
  .section-box {
    background: #131b2e; border: 1px solid #1e293b; border-radius: 12px;
    padding: 10px; margin-bottom: 10px; text-align: left;
  }
  .section-box summary {
    font-size: 0.9rem; font-weight: 700; color: #94a3b8; cursor: pointer; outline: none;
  }
  .cfg-grid { display: grid; grid-template-columns: repeat(2, 1fr); gap: 6px; margin-top: 8px; }
  .cfg-btn {
    height: 40px; font-size: 0.8rem; background: #1e293b; color: #e2e8f0; border-radius: 6px;
    display: flex; align-items: center; justify-content: center; text-align: center;
  }
  .cfg-btn.active { background: #0284c7; color: #fff; font-weight: 800; border: 1px solid #38bdf8; }
  .cfg-btn.break { background: #4338ca; color: #c7d2fe; }
  .cfg-btn.danger { background: #991b1b; color: #fecaca; grid-column: span 2; font-size: 0.85rem; }
  .color-bar { display: flex; gap: 6px; align-items: center; margin-bottom: 8px; flex-wrap: wrap; }
  .c-dot { width: 32px; height: 32px; border-radius: 50%; border: 2px solid #334155; cursor: pointer; transition: transform 0.1s; }
  .c-dot:active { transform: scale(1.15); border-color: #fff; }
</style>
</head>
<body>

  <span id="badgeLink" class="badge warn">Conectando...</span>
  <h1 id="sportName">TABLERO DEPORTIVO</h1>

  <!-- CRONOMETRO -->
  <div class="clock-card">
    <div id="periodBadge" class="period-badge">PERÍODO 1</div>
    <div id="clockDisplay" class="clock-display">01:00</div>
    <button id="btnTimer" class="btn-timer start" onclick="toggleTimer()">▶ INICIAR TIEMPO</button>
  </div>

  <!-- MARCADOR LOCAL Y VISITA -->
  <div class="teams-grid">
    <!-- LOCAL -->
    <div class="team-card local">
      <div class="team-title">LOCAL</div>
      <div id="scoreHome" class="score-num">00</div>
      <div class="fouls-row">Faltas: <b id="foulsHome">0</b></div>
      <div class="btn-row">
        <button class="btn-pt main" onclick="sendPt('h', 1)">+1</button>
        <button class="btn-pt main" onclick="sendPt('h', 2)">+2</button>
        <button class="btn-pt main" onclick="sendPt('h', 3)">+3</button>
        <button class="btn-pt sub" onclick="sendPt('h', -1)">-1</button>
      </div>
      <div class="btn-row">
        <button class="btn-foul" onclick="send(8)">+ Falta</button>
        <button class="btn-foul" onclick="send(9)">- Falta</button>
      </div>
    </div>

    <!-- VISITA -->
    <div class="team-card visita">
      <div class="team-title">VISITA</div>
      <div id="scoreAway" class="score-num">00</div>
      <div class="fouls-row">Faltas: <b id="foulsAway">0</b></div>
      <div class="btn-row">
        <button class="btn-pt main" onclick="sendPt('a', 1)">+1</button>
        <button class="btn-pt main" onclick="sendPt('a', 2)">+2</button>
        <button class="btn-pt main" onclick="sendPt('a', 3)">+3</button>
        <button class="btn-pt sub" onclick="sendPt('a', -1)">-1</button>
      </div>
      <div class="btn-row">
        <button class="btn-foul" onclick="send(10)">+ Falta</button>
        <button class="btn-foul" onclick="send(11)">- Falta</button>
      </div>
    </div>
  </div>

  <!-- CONTROL DE PERIODOS Y BOCINA -->
  <div class="btn-row" style="margin-bottom: 12px;">
    <button class="btn-pt" style="background:#0369a1; height:44px;" onclick="send(6)">+ Período</button>
    <button class="btn-pt" style="background:#075985; height:44px;" onclick="send(7)">- Período</button>
    <button class="btn-pt" style="background:#b91c1c; height:44px;" onclick="send(15, 1500)">🔊 Bocina</button>
  </div>

  <!-- CONFIGURACIÓN DE CUARTOS Y ENTRETIEMPOS -->
  <details class="section-box" open>
    <summary>⚙️ Ajustes de Juego, Cuartos y Descansos</summary>
    
    <p style="font-size:0.75rem; color:#94a3b8; margin:8px 0 4px;">DEPORTE REGLAMENTARIO:</p>
    <div class="cfg-grid">
      <button id="btnModoBasket" class="cfg-btn" onclick="send(18, 0)">🏀 BÁSQUET (10m / 5F)</button>
      <button id="btnModoFutsal" class="cfg-btn" onclick="send(18, 1)">⚽ FUTSAL (20m / 6F)</button>
    </div>

    <p style="font-size:0.75rem; color:#94a3b8; margin:10px 0 4px;">DURACIÓN DEL PERÍODO / CUARTO:</p>
    <div class="cfg-grid">
      <button class="cfg-btn" onclick="setTimeSec(60)">1 Minuto (01:00)</button>
      <button class="cfg-btn" onclick="setTimeSec(600)">10 Min (Básquet)</button>
      <button class="cfg-btn" onclick="setTimeSec(720)">12 Min (NBA)</button>
      <button class="cfg-btn" onclick="setTimeSec(1200)">20 Minutos</button>
    </div>

    <p style="font-size:0.75rem; color:#94a3b8; margin:10px 0 4px;">ENTRETIEMPO / DESCANSO (LETRA 'E' EN PERÍODO):</p>
    <div class="cfg-grid">
      <button class="cfg-btn break" onclick="setTimeSec(-60)">⏱️ E - 1 min</button>
      <button class="cfg-btn break" onclick="setTimeSec(-120)">⏱️ E - 2 min</button>
      <button class="cfg-btn break" onclick="setTimeSec(-300)">⏱️ E - 5 min</button>
      <button class="cfg-btn break" onclick="setTimeSec(-900)">⏱️ E - 15 min</button>
    </div>

    <p style="font-size:0.75rem; color:#94a3b8; margin:10px 0 4px;">COLORES TANTEADOR LOCAL:</p>
    <div class="color-bar">
      <button class="c-dot" style="background:#ff8200" title="Ámbar" onclick="send(19, 1)"></button>
      <button class="c-dot" style="background:#00dc28" title="Verde" onclick="send(19, 2)"></button>
      <button class="c-dot" style="background:#00a0ff" title="Celeste" onclick="send(19, 3)"></button>
      <button class="c-dot" style="background:#ff0000" title="Rojo" onclick="send(19, 4)"></button>
      <button class="c-dot" style="background:#ffffff" title="Blanco" onclick="send(19, 5)"></button>
      <button class="c-dot" style="background:#ffe600" title="Amarillo" onclick="send(19, 6)"></button>
      <button class="c-dot" style="background:#c800ff" title="Violeta" onclick="send(19, 7)"></button>
      <button class="cfg-btn" style="flex:1; height:32px; font-size:0.75rem; min-width:65px;" onclick="send(19, 0)">Ciclar ↺</button>
    </div>

    <p style="font-size:0.75rem; color:#94a3b8; margin:10px 0 4px;">COLORES TANTEADOR VISITA:</p>
    <div class="color-bar">
      <button class="c-dot" style="background:#ff8200" title="Ámbar" onclick="send(20, 1)"></button>
      <button class="c-dot" style="background:#00dc28" title="Verde" onclick="send(20, 2)"></button>
      <button class="c-dot" style="background:#00a0ff" title="Celeste" onclick="send(20, 3)"></button>
      <button class="c-dot" style="background:#ff0000" title="Rojo" onclick="send(20, 4)"></button>
      <button class="c-dot" style="background:#ffffff" title="Blanco" onclick="send(20, 5)"></button>
      <button class="c-dot" style="background:#ffe600" title="Amarillo" onclick="send(20, 6)"></button>
      <button class="c-dot" style="background:#c800ff" title="Violeta" onclick="send(20, 7)"></button>
      <button class="cfg-btn" style="flex:1; height:32px; font-size:0.75rem; min-width:65px;" onclick="send(20, 0)">Ciclar ↺</button>
    </div>

    <p style="font-size:0.75rem; color:#94a3b8; margin:10px 0 4px;">COLOR PERÍODO / ENTRETIEMPO (E):</p>
    <div class="color-bar">
      <button class="c-dot" style="background:#ff8200" title="Ámbar" onclick="send(23, 1)"></button>
      <button class="c-dot" style="background:#00dc28" title="Verde" onclick="send(23, 2)"></button>
      <button class="c-dot" style="background:#00a0ff" title="Celeste" onclick="send(23, 3)"></button>
      <button class="c-dot" style="background:#ff0000" title="Rojo" onclick="send(23, 4)"></button>
      <button class="c-dot" style="background:#ffffff" title="Blanco" onclick="send(23, 5)"></button>
      <button class="c-dot" style="background:#ffe600" title="Amarillo" onclick="send(23, 6)"></button>
      <button class="c-dot" style="background:#c800ff" title="Violeta" onclick="send(23, 7)"></button>
      <button class="cfg-btn" style="flex:1; height:32px; font-size:0.75rem; min-width:65px;" onclick="send(23, 0)">Ciclar ↺</button>
    </div>

    <p style="font-size:0.75rem; color:#94a3b8; margin:10px 0 4px;">COLOR RELOJ / TIEMPO:</p>
    <div class="color-bar">
      <button class="c-dot" style="background:#ff8200" title="Ámbar" onclick="send(24, 1)"></button>
      <button class="c-dot" style="background:#00dc28" title="Verde" onclick="send(24, 2)"></button>
      <button class="c-dot" style="background:#00a0ff" title="Celeste" onclick="send(24, 3)"></button>
      <button class="c-dot" style="background:#ff0000" title="Rojo" onclick="send(24, 4)"></button>
      <button class="c-dot" style="background:#ffffff" title="Blanco" onclick="send(24, 5)"></button>
      <button class="c-dot" style="background:#ffe600" title="Amarillo" onclick="send(24, 6)"></button>
      <button class="c-dot" style="background:#c800ff" title="Violeta" onclick="send(24, 7)"></button>
      <button class="cfg-btn" style="flex:1; height:32px; font-size:0.75rem; min-width:65px;" onclick="send(24, 0)">Ciclar ↺</button>
    </div>

    <div class="cfg-grid" style="margin-top:12px;">
      <button class="cfg-btn danger" onclick="if(confirm('¿Reiniciar todo el partido a cero?')) send(14)">🔄 REINICIAR PARTIDO A CERO (0-0)</button>
    </div>

    <div style="margin-top:14px; text-align:center;">
      <a href="/update" style="color:#38bdf8; font-size:0.82rem; text-decoration:none; font-weight:600;">📦 Actualizar Firmware (.bin OTA)</a>
    </div>
  </details>

<script>
let isRunning = false;

async function send(cmd, val = 0) {
  try {
    const res = await fetch(`/cmd?c=${cmd}&v=${val}`, { method: 'POST' });
    if (res.ok) {
      const data = await res.json();
      updateUI(data);
    }
  } catch(e) {
    console.error(e);
  }
}

async function sendPt(team, pts) {
  if (pts === 1) await send(team === 'h' ? 2 : 4);
  else if (pts === 2) { await send(team === 'h' ? 2 : 4); await send(team === 'h' ? 2 : 4); }
  else if (pts === 3) { await send(team === 'h' ? 2 : 4); await send(team === 'h' ? 2 : 4); await send(team === 'h' ? 2 : 4); }
  else if (pts === -1) await send(team === 'h' ? 3 : 5);
}

function setTimeSec(sec) {
  // CMD_SET_EXACT_SECONDS = 22
  send(22, sec);
}

function toggleTimer() {
  send(1); // CMD_TIMER_TOGGLE
}

function updateUI(d) {
  isRunning = d.run;
  document.getElementById('badgeLink').className = d.ack ? 'badge ok' : 'badge warn';
  document.getElementById('badgeLink').innerText = d.ack ? 'ENLAZADO CON TABLERO' : 'BUSCANDO TABLERO...';

  document.getElementById('sportName').innerText = d.futsal ? 'TABLERO FUTSAL' : 'TABLERO BÁSQUETBOL';
  document.getElementById('btnModoBasket').className = d.futsal ? 'cfg-btn' : 'cfg-btn active';
  document.getElementById('btnModoFutsal').className = d.futsal ? 'cfg-btn active' : 'cfg-btn';

  document.getElementById('periodBadge').innerText = d.halftime ? 'ENTRETIEMPO (E)' : ('PERÍODO ' + d.p);
  document.getElementById('scoreHome').innerText = String(d.h).padStart(2, '0');
  document.getElementById('scoreAway').innerText = String(d.a).padStart(2, '0');
  document.getElementById('foulsHome').innerText = d.hf;
  document.getElementById('foulsAway').innerText = d.af;

  localSec = d.m * 60 + d.s;
  renderClock();
  
  const btn = document.getElementById('btnTimer');
  if (d.run) {
    btn.className = 'btn-timer pause';
    btn.innerText = '⏸ PAUSAR TIEMPO';
    document.getElementById('clockDisplay').className = 'clock-display running';
  } else {
    btn.className = 'btn-timer start';
    btn.innerText = '▶ INICIAR TIEMPO';
    document.getElementById('clockDisplay').className = 'clock-display';
  }
}

let localSec = 60;
function renderClock() {
  const mm = String(Math.floor(localSec / 60)).padStart(2, '0');
  const ss = String(localSec % 60).padStart(2, '0');
  document.getElementById('clockDisplay').innerText = `${mm}:${ss}`;
}

setInterval(() => {
  if (isRunning && localSec > 0) {
    localSec--;
    renderClock();
  }
}, 1000);

async function pollStatus() {
  try {
    const res = await fetch('/status');
    if (res.ok) {
      const data = await res.json();
      updateUI(data);
    }
  } catch(e) {}
}

setInterval(pollStatus, 1200);
pollStatus();
</script>

</body>
</html>
)rawliteral";

void handleRoot() {
  server.send_P(200, "text/html; charset=utf-8", PAGE_INDEX);
}

void handleStatus() {
  uint32_t totalSec = lastState.remainingMs / 1000;
  uint32_t m = totalSec / 60;
  uint32_t s = totalSec % 60;
  uint32_t ds = (lastState.remainingMs % 1000) / 100;
  bool running = (lastState.flags & STATE_RUNNING) != 0;
  bool isFutsal = (lastState.flags & STATE_FUTSAL_MODE) != 0;
  bool isHalftime = (lastState.flags & STATE_HALFTIME_MODE) != 0;

  char json[280];
  snprintf(json, sizeof(json),
           "{\"h\":%u,\"a\":%u,\"p\":%u,\"hf\":%u,\"af\":%u,\"m\":%lu,\"s\":%"
           "lu,\"ds\":%lu,\"run\":%s,\"futsal\":%s,\"halftime\":%s,\"ack\":%s}",
           lastState.home, lastState.away, lastState.period,
           lastState.homeFouls, lastState.awayFouls, (unsigned long)m,
           (unsigned long)s, (unsigned long)ds, running ? "true" : "false",
           isFutsal ? "true" : "false", isHalftime ? "true" : "false",
           ackReceived ? "true" : "false");
  server.send(200, "application/json", json);
}

void handleCmd() {
  if (!server.hasArg("c")) {
    server.send(400, "text/plain", "Missing c");
    return;
  }
  int cmd = server.arg("c").toInt();
  int val = server.hasArg("v") ? server.arg("v").toInt() : 0;

  bool ok = sendCommand((Command)cmd, (int16_t)val);
  blink(ok ? 1 : 3);
  handleStatus();
}

// =====================================================================
// SETUP & LOOP
// =====================================================================
void setup() {
  Serial.begin(115200);
  pinMode(STATUS_LED, OUTPUT);
  digitalWrite(STATUS_LED, LOW);
  pinMode(PIN_BTN_BACK, INPUT_PULLUP);
  for (auto &b : buttons)
    pinMode(b.pin, INPUT_PULLUP);

  // Sesión nueva en cada arranque (anti-replay): contador persistente en NVS
  prefs.begin("remote", false);
  session = prefs.getUInt("session", 0) + 1;
  prefs.putUInt("session", session);

  // Modo mixto: SoftAP para el celular + Station para ESP-NOW
  WiFi.mode(WIFI_AP_STA);
  WiFi.disconnect();

  // Levanta el Access Point Wi-Fi para que se conecte el celular
  // Canal ESPNOW_CHANNEL (1) obligatorio para coexistir con ESP-NOW
  // Nombre discreto "ZenTX-B", clave WPA2 y límite de 3 conexiones simultáneas
  WiFi.softAP("ZenTX-B", "12345678", ESPNOW_CHANNEL, 0, 3);
  esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
  esp_wifi_set_max_tx_power(WIFI_POWER_8_5dBm);
  WiFi.setSleep(false);

  Serial.println("=========================================");
  Serial.println("  CONTROL REMOTO + INTERFAZ CELULAR");
  Serial.print("  Wi-Fi SSID: ZenTX-B\n  Clave:      12345678\n  IP:         ");
  Serial.println(WiFi.softAPIP());
  Serial.print("  MAC remoto: ");
  Serial.println(WiFi.macAddress());
  Serial.println("=========================================");

  // DNS Server para Captive Portal (redirige cualquier dominio a 192.168.4.1)
  dnsServer.start(DNS_PORT, "*", WiFi.softAPIP());

  // Servidor Web
  server.on("/", HTTP_GET, handleRoot);
  server.on("/status", HTTP_GET, handleStatus);
  server.on("/cmd", handleCmd);

  // Servidor OTA para subir firmware.bin desde el navegador
  server.on("/update", HTTP_GET, []() {
    server.send(
        200, "text/html; charset=utf-8",
        "<!DOCTYPE html><html><head><meta charset='utf-8'><meta "
        "name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>Actualizar Firmware OTA - ZenTX</title>"
        "<style>body{background:#090d16;color:#f8fafc;font-family:sans-serif;"
        "padding:16px;text-align:center;}"
        ".card{background:#131b2e;border:1px solid "
        "#1e293b;border-radius:14px;padding:20px;max-width:420px;margin:20px "
        "auto;box-shadow:0 4px 14px rgba(0,0,0,0.5);}"
        "input[type=file]{margin:16px "
        "0;width:100%;color:#94a3b8;font-size:0.9rem;}"
        "input[type=submit]{background:#0284c7;color:#fff;border:none;padding:"
        "12px "
        "24px;border-radius:8px;font-weight:bold;cursor:pointer;width:100%;"
        "font-size:1rem;}"
        ".back{display:inline-block;margin-top:16px;color:#38bdf8;text-"
        "decoration:none;font-size:0.85rem;}"
        "#prg{display:none;margin-top:16px;font-weight:bold;color:#34d399;}"
        "</style></head><body>"
        "<div class='card'><h2>📦 Actualización Inalámbrica (OTA)</h2>"
        "<p style='color:#94a3b8;font-size:0.85rem;'>Selecciona el archivo "
        "<b>firmware.bin</b> compilado:</p>"
        "<form method='POST' action='/update' enctype='multipart/form-data' "
        "onsubmit=\"document.getElementById('prg').style.display='block';\">"
        "<input type='file' name='update' accept='.bin' required><br>"
        "<input type='submit' value='Subir y Flashear Firmware'>"
        "</form><div id='prg'>⏳ Flasheando firmware... Espera unos segundos "
        "sin apagar el equipo.</div>"
        "<a href='/' class='back'>← Volver al Marcador</a>"
        "</div></body></html>");
  });

  server.on(
      "/update", HTTP_POST,
      []() {
        server.sendHeader("Connection", "close");
        server.send(
            200, "text/html; charset=utf-8",
            Update.hasError()
                ? "<h3>❌ Error en actualización</h3><p>Verifica el archivo "
                  ".bin</p><a href='/update'>Reintentar</a>"
                : "<h3>✅ ¡Actualización Exitosa!</h3><p>Reiniciando el "
                  "control...</p><script>setTimeout(()=>window.location.href='/"
                  "', 6000);</script>");
        delay(500);
        ESP.restart();
      },
      []() {
        HTTPUpload &upload = server.upload();
        if (upload.status == UPLOAD_FILE_START) {
          Serial.printf("[OTA] Inicio: %s\n", upload.filename.c_str());
          if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
            Update.printError(Serial);
          }
        } else if (upload.status == UPLOAD_FILE_WRITE) {
          if (Update.write(upload.buf, upload.currentSize) !=
              upload.currentSize) {
            Update.printError(Serial);
          }
        } else if (upload.status == UPLOAD_FILE_END) {
          if (Update.end(true)) {
            Serial.printf("[OTA] Completado: %u bytes\n", upload.totalSize);
          } else {
            Update.printError(Serial);
          }
        }
      });

  server.onNotFound(handleRoot); // Redirige captive portal
  server.begin();

  // Inicializa ESP-NOW
  if (esp_now_init() != ESP_OK) {
    Serial.println("Error ESP-NOW");
    return;
  }
  esp_now_set_pmk(ESPNOW_PMK);
  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, BOARD_MAC, 6);
  memcpy(peer.lmk, ESPNOW_LMK, 16);
  peer.channel = ESPNOW_CHANNEL;
  peer.encrypt = true;
  if (esp_now_add_peer(&peer) != ESP_OK) {
    Serial.println("Error al agregar peer (revisar BOARD_MAC en secrets.h)");
  }
  esp_now_register_recv_cb(onReceive);

  blink(sendCommand(CMD_SYNC, 0) ? 1 : 3);
}

void loop() {
  // 1. Atiende peticiones del celular (Captive Portal + Web Server)
  dnsServer.processNextRequest();
  server.handleClient();

  // 2. Escanea botones físicos en el protoboard / caja
  uint32_t now = millis();
  bool isBackActive = (digitalRead(PIN_BTN_BACK) == LOW);
  static uint32_t lastCmdTime = 0;

  for (auto &b : buttons) {
    bool state = digitalRead(b.pin);

    // Detección en vivo de pulsación sostenida (>1.5s) sin esperar a soltar
    if (!b.lastState && !state && !b.longFired && (now - b.pressedAt >= 1500)) {
      bool useShift = isBackActive || b.shiftOnPress;
      bool handledLong = false;
      Command cmdLong = CMD_SYNC;
      int16_t valLong = 0;

      if (b.pin == PIN_BTN_HOME) {
        // Sostener Local (con o sin Shift) -> Ciclar Color Local
        cmdLong = CMD_COLOR_CYCLE_HOME;
        handledLong = true;
      } else if (b.pin == PIN_BTN_AWAY) {
        // Sostener Visita (con o sin Shift) -> Ciclar Color Visita
        cmdLong = CMD_COLOR_CYCLE_AWAY;
        handledLong = true;
      } else if (b.pin == PIN_BTN_TIMER && useShift) {
        // Sostener Tiempo + Shift -> Ciclar Duración (1, 2, 5, 10, 12, 15, 20 min)
        cmdLong = CMD_TIME_CYCLE_PRESET;
        handledLong = true;
      } else if (b.pin == PIN_BTN_PER && useShift) {
        // Sostener Período + Shift -> Reiniciar Partido Completo a Cero (00-00)
        cmdLong = CMD_GAME_RESET;
        handledLong = true;
      } else if (b.pin == PIN_BTN_PER && !useShift) {
        // Sostener Período (sin Shift) -> Activar/Desactivar Entretiempo ('E')
        cmdLong = CMD_HALFTIME_TOGGLE;
        valLong = 60;
        handledLong = true;
      }

      if (handledLong) {
        b.longFired = true;
        lastCmdTime = now;
        bool ok = sendCommand(cmdLong, valLong);
        blink(ok ? 2 : 3);
      }
    }

    // Debounce de 60ms para estabilidad total contra falsos contactos o ruido
    // de cables
    if (state != b.lastState && now - b.lastChange > 60) {
      b.lastChange = now;
      b.lastState = state;
      if (!state) {
        b.pressedAt = now;
        b.shiftOnPress = isBackActive;
        b.longFired = false;
      } else {
        if (b.longFired) {
          b.longFired = false;
          continue;
        }
        uint32_t pressDuration = now - b.pressedAt;
        // Filtro antirruido: ignorar transitorios menores a 45ms
        if (pressDuration < 45)
          continue;

        // Rate limiter anti-ametralladora: no más de 1 comando cada 150ms
        if (now - lastCmdTime < 150)
          continue;
        lastCmdTime = now;

        bool useShift = isBackActive || b.shiftOnPress;
        Command cmd = useShift ? b.cmdBack : b.cmdNormal;
        int16_t val = useShift ? b.valueBack : b.valueNormal;

        bool ok = sendCommand(cmd, val);
        blink(ok ? 1 : 3);
      }
    }
  }
}
