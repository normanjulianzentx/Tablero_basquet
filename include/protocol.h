#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>

// =====================================================================
// Protocolo ESP-NOW v2 (control remoto <-> tablero deportivo)
//
// Capas de seguridad:
//  1. Cifrado ESP-NOW (AES-CCM) con PMK/LMK secretos + unicast a MAC conocida.
//  2. Validación de aplicación: magic + versión + tipo + CRC16.
//  3. Anti-replay: `session` (persistente en NVS) + `seq` creciente.
//  4. ACK obligatorio: el tablero responde con su ESTADO COMPLETO.
// =====================================================================

#define ESPNOW_CHANNEL        1
#define PROTOCOL_MAGIC        0xA5
#define PROTOCOL_VERSION      2

enum PacketType : uint8_t { PKT_COMMAND = 1, PKT_STATE = 2 };

enum Command : uint8_t {
  CMD_SYNC = 0,           // No cambia nada; solo pide el estado actual
  CMD_TIMER_TOGGLE,       // Start / Stop del reloj (un solo botón)
  CMD_HOME_INC,
  CMD_HOME_DEC,
  CMD_AWAY_INC,
  CMD_AWAY_DEC,
  CMD_PERIOD_INC,         // Avanza período (cicla 1..9)
  CMD_PERIOD_DEC,         // Retrocede período (cicla 9..1)
  CMD_FOUL_HOME_INC,      // Falta Local +1
  CMD_FOUL_HOME_DEC,      // Falta Local -1
  CMD_FOUL_AWAY_INC,      // Falta Visita +1
  CMD_FOUL_AWAY_DEC,      // Falta Visita -1
  CMD_TIME_ADD,           // Suma `value` segundos (puede ser negativo)
  CMD_TIME_RESET,         // Reloj al valor reglamentario del deporte actual
  CMD_GAME_RESET,         // Todo a cero
  CMD_HORN,               // Bocina manual `value` milisegundos (100..5000)
  CMD_TEST_MODE,          // Modo de prueba: recorre dígitos contando 0..8 y ciclo de colores
  CMD_SPORT_TOGGLE,       // Alterna deporte: Básquet <-> Futsal
  CMD_SET_SPORT_MODE,     // Fija deporte: 0 = Básquet, 1 = Futsal
  CMD_COLOR_CYCLE_HOME,   // Cicla o fija color del equipo LOCAL (1..7 o 0=ciclar)
  CMD_COLOR_CYCLE_AWAY,   // Cicla o fija color del equipo VISITA (1..7 o 0=ciclar)
  CMD_TIME_CYCLE_PRESET,  // Cicla tiempos predefinidos (1 -> 2 -> 5 -> 10 -> 12 -> 15 -> 20 min)
  CMD_SET_EXACT_SECONDS,  // Fija el tiempo en segundos exactos (>0 = cuarto, <0 = entretiempo 'E')
  CMD_COLOR_CYCLE_PERIOD, // Cicla o fija color del PERÍODO (1..7 o 0=ciclar)
  CMD_COLOR_CYCLE_TIME,   // Cicla o fija color del TIEMPO (1..7 o 0=ciclar)
  CMD_HALFTIME_TOGGLE     // Activa / Desactiva modo Entretiempo ('E' en Período)
};

enum StateFlags : uint8_t {
  STATE_RUNNING       = 1 << 0,
  STATE_HORN          = 1 << 1,
  STATE_FUTSAL_MODE   = 1 << 2, // 0 = Básquet, 1 = Futsal
  STATE_HALFTIME_MODE = 1 << 3  // 1 = Entretiempo ('E' en dígito de Período)
};

struct __attribute__((packed)) CmdPacket {
  uint8_t  magic;
  uint8_t  version;
  uint8_t  type;          // PKT_COMMAND
  uint8_t  cmd;
  int16_t  value;
  uint32_t session;
  uint32_t seq;
  uint16_t crc;           // SIEMPRE el último campo
};

struct __attribute__((packed)) StatePacket {
  uint8_t  magic;
  uint8_t  version;
  uint8_t  type;          // PKT_STATE
  uint8_t  home;
  uint8_t  away;
  uint8_t  period;
  uint8_t  homeFouls;     // Faltas equipo local
  uint8_t  awayFouls;     // Faltas equipo visita
  uint8_t  flags;         // StateFlags (STATE_RUNNING, STATE_HORN, STATE_FUTSAL_MODE)
  uint32_t remainingMs;
  uint32_t ackSession;    // eco del comando confirmado
  uint32_t ackSeq;
  uint16_t crc;           // SIEMPRE el último campo
};

// CRC-16/CCITT-FALSE
inline uint16_t crc16(const uint8_t *d, size_t n) {
  uint16_t c = 0xFFFF;
  while (n--) {
    c ^= (uint16_t)(*d++) << 8;
    for (uint8_t i = 0; i < 8; i++) c = (c & 0x8000) ? (c << 1) ^ 0x1021 : (c << 1);
  }
  return c;
}

template <typename T> inline void sealPacket(T &p) {
  p.crc = crc16((const uint8_t *)&p, sizeof(T) - sizeof(uint16_t));
}

// Copia a `out` solo si longitud, magic, versión, tipo y CRC son correctos.
template <typename T> inline bool parsePacket(const uint8_t *data, int len, uint8_t type, T &out) {
  if (len != (int)sizeof(T)) return false;
  memcpy(&out, data, sizeof(T));
  if (out.magic != PROTOCOL_MAGIC || out.version != PROTOCOL_VERSION || out.type != type) return false;
  return out.crc == crc16(data, sizeof(T) - sizeof(uint16_t));
}
