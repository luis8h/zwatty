// Fitness Machine Service (0x1826), Indoor Bike profile.
// Spec: Bluetooth SIG "Fitness Machine Service 1.0" (FTMS).
#include "ftms.h"
#include <NimBLEDevice.h>
#include <esp_system.h>
#include <math.h>
#include "config.h"
#include "sim.h"

namespace {

// ---- GATT UUIDs -----------------------------------------------------------
constexpr uint16_t UUID_FTMS            = 0x1826;
constexpr uint16_t UUID_FEATURE         = 0x2ACC;
constexpr uint16_t UUID_INDOOR_BIKE     = 0x2AD2;
constexpr uint16_t UUID_TRAINING_STATUS = 0x2AD3;
constexpr uint16_t UUID_RESISTANCE_RNG  = 0x2AD6;
constexpr uint16_t UUID_POWER_RNG       = 0x2AD8;
constexpr uint16_t UUID_CONTROL_POINT   = 0x2AD9;
constexpr uint16_t UUID_MACHINE_STATUS  = 0x2ADA;

// ---- Control point opcodes / result codes / machine status opcodes ---------
enum : uint8_t {
  OP_REQUEST_CONTROL = 0x00, OP_RESET = 0x01, OP_TARGET_RESISTANCE = 0x04,
  OP_TARGET_POWER = 0x05, OP_START = 0x07, OP_STOP = 0x08,
  OP_SIM_PARAMS = 0x11, OP_RESPONSE = 0x80
};
enum : uint8_t {
  RES_SUCCESS = 0x01, RES_NOT_SUPPORTED = 0x02, RES_INVALID_PARAM = 0x03,
  RES_FAILED = 0x04, RES_NOT_PERMITTED = 0x05
};
enum : uint8_t {
  MS_RESET = 0x01, MS_STOPPED = 0x02, MS_STARTED = 0x04,
  MS_TARGET_RESISTANCE = 0x07, MS_TARGET_POWER = 0x08, MS_SIM_PARAMS = 0x12
};
enum : uint8_t { TS_IDLE = 0x01, TS_MANUAL = 0x0D };   // Training Status values

NimBLEServer* gServer = nullptr;
NimBLECharacteristic *chBike = nullptr, *chTraining = nullptr, *chCp = nullptr, *chStatus = nullptr;

inline void put16(uint8_t* b, uint16_t v) { b[0] = v & 0xFF; b[1] = v >> 8; }

// ---- Outgoing queue ---------------------------------------------------------
// Control point responses and status notifications are queued and sent from the
// Arduino loop task. This lets us delay or drop them for testing, and keeps
// BLE calls out of the host-task callbacks. FIFO, so ordering is preserved.
struct Pending {
  uint32_t due; NimBLECharacteristic* ch; bool indicate;
  uint8_t len; uint8_t tries; uint32_t epoch; uint8_t data[10];
};
constexpr size_t QN = 12;
Pending q[QN];
size_t qHead = 0, qCount = 0;
uint32_t qEpoch = 0;

// All queue helpers below expect the SimLock to be held.
void clearQueue() { qHead = 0; qCount = 0; qEpoch++; }

void enqueue(NimBLECharacteristic* ch, bool indicate, const uint8_t* d, uint8_t n) {
  if (qCount >= QN || n > sizeof(Pending::data)) return;
  Pending& p = q[(qHead + qCount) % QN];
  p.due = millis() + (uint32_t)sim.cfg.ctrlDelayMs;
  p.ch = ch; p.indicate = indicate; p.len = n; p.tries = 0; p.epoch = qEpoch;
  memcpy(p.data, d, n);
  qCount++;
}

void respond(uint8_t op, uint8_t result) {
  if (sim.cfg.ctrlSilent >= 0.5f) { sim.logf("CP -> (suppressed) op 0x%02X", op); return; }
  const uint8_t r[3] = { OP_RESPONSE, op, result };
  enqueue(chCp, true, r, 3);
  sim.logf("CP -> op 0x%02X result 0x%02X", op, result);
}

void machineStatus(const uint8_t* d, uint8_t n) { enqueue(chStatus, false, d, n); }

void trainingStatus(uint8_t s) {
  const uint8_t d[2] = { 0x00, s };
  chTraining->setValue(d, 2);        // keep the readable value current
  enqueue(chTraining, false, d, 2);
}

void drainQueue() {
  for (;;) {
    Pending p;
    {
      SimLock l;
      if (qCount == 0) return;
      if ((int32_t)(millis() - q[qHead].due) < 0) return;
      p = q[qHead];
    }
    p.ch->setValue(p.data, p.len);
    if (p.indicate) p.ch->indicate(); else p.ch->notify();   // both return void in NimBLE-Arduino 1.4.x
    SimLock l;
    if (p.epoch != qEpoch) continue;                        // queue was cleared meanwhile
    qHead = (qHead + 1) % QN;
    qCount--;
  }
}

// ---- Indoor Bike Data -------------------------------------------------------
// Flags 0x0854: instantaneous cadence | total distance | instantaneous power | elapsed time
// (instantaneous speed is present because "more data" bit 0 is 0).
// Layout: flags u16 | speed u16 0.01km/h | cadence u16 0.5rpm | distance u24 m | power s16 W | elapsed u16 s
size_t buildBikeData(uint8_t* b) {   // SimLock held
  const SimState& s = sim.st;
  auto clampf = [](float v, float hi) { return v < 0 ? 0.0f : (v > hi ? hi : v); };
  put16(b + 0, 0x0854);
  put16(b + 2, (uint16_t)lroundf(clampf(s.speed * 3.6f * 100.0f, 65535)));
  put16(b + 4, (uint16_t)lroundf(clampf(s.cadenceOut * 2.0f, 65535)));
  uint32_t dist = (uint32_t)clampf(s.distance, 16777215);
  b[6] = dist & 0xFF; b[7] = (dist >> 8) & 0xFF; b[8] = (dist >> 16) & 0xFF;
  put16(b + 9, (uint16_t)(int16_t)lroundf(clampf(s.powerOut, 32767)));
  put16(b + 11, (uint16_t)clampf(s.elapsed, 65535));
  return 13;
}

// ---- Control point ----------------------------------------------------------
void toHex(const uint8_t* d, size_t n, char* out, size_t outSize) {
  size_t pos = 0;
  for (size_t i = 0; i < n && pos + 3 < outSize; i++) pos += snprintf(out + pos, outSize - pos, "%02X ", d[i]);
  if (pos) out[pos - 1] = 0; else out[0] = 0;
}

void handleControlPoint(const uint8_t* d, size_t n) {
  if (n == 0) return;
  SimLock l;
  SimState& s = sim.st;
  char hex[48];
  toHex(d, n, hex, sizeof(hex));
  sim.logf("CP <- %s", hex);
  const uint8_t op = d[0];

  // Failure injection: answer with the configured error, change nothing.
  if (sim.cfg.failNext >= 1.0f) {
    sim.cfg.failNext -= 1.0f;
    sim.logf("injected failure for op 0x%02X", op);
    respond(op, (uint8_t)sim.cfg.failCode);
    return;
  }

  const bool known = op == OP_REQUEST_CONTROL || op == OP_RESET || op == OP_TARGET_RESISTANCE ||
                     op == OP_TARGET_POWER || op == OP_START || op == OP_STOP || op == OP_SIM_PARAMS;
  if (!known) { respond(op, RES_NOT_SUPPORTED); return; }
  if (op != OP_REQUEST_CONTROL && !s.haveControl) { respond(op, RES_NOT_PERMITTED); return; }

  switch (op) {
    case OP_REQUEST_CONTROL:
      s.haveControl = true;
      respond(op, RES_SUCCESS);
      break;

    case OP_RESET: {
      s.running = false; s.mode = Mode::Free; s.targetPower = 0; s.resistance = 0;
      s.grade = 0; s.wind = 0; s.elapsed = 0;
      respond(op, RES_SUCCESS);
      const uint8_t m[1] = { MS_RESET };
      machineStatus(m, 1);
      trainingStatus(TS_IDLE);
      break;
    }

    case OP_TARGET_RESISTANCE: {
      if (n < 2 || d[1] > RESISTANCE_MAX) { respond(op, RES_INVALID_PARAM); break; }
      s.resistance = d[1]; s.mode = Mode::Resistance;
      sim.logf("resistance level %u.%u", d[1] / 10, d[1] % 10);
      respond(op, RES_SUCCESS);
      const uint8_t m[2] = { MS_TARGET_RESISTANCE, d[1] };
      machineStatus(m, 2);
      break;
    }

    case OP_TARGET_POWER: {
      if (n < 3) { respond(op, RES_INVALID_PARAM); break; }
      int16_t w = (int16_t)(d[1] | (d[2] << 8));
      if (w < POWER_RANGE_MIN || w > POWER_RANGE_MAX) { respond(op, RES_INVALID_PARAM); break; }
      s.targetPower = w; s.mode = Mode::Erg;
      sim.logf("ERG target %d W", w);
      respond(op, RES_SUCCESS);
      const uint8_t m[3] = { MS_TARGET_POWER, d[1], d[2] };
      machineStatus(m, 3);
      break;
    }

    case OP_START: {
      s.running = true;
      sim.logf("start/resume");
      respond(op, RES_SUCCESS);
      const uint8_t m[1] = { MS_STARTED };
      machineStatus(m, 1);
      trainingStatus(TS_MANUAL);
      break;
    }

    case OP_STOP: {
      if (n < 2 || (d[1] != 0x01 && d[1] != 0x02)) { respond(op, RES_INVALID_PARAM); break; }
      s.running = false;
      sim.logf("%s", d[1] == 0x01 ? "stop" : "pause");
      respond(op, RES_SUCCESS);
      const uint8_t m[2] = { MS_STOPPED, d[1] };
      machineStatus(m, 2);
      trainingStatus(TS_IDLE);
      break;
    }

    case OP_SIM_PARAMS: {   // wind s16 0.001 m/s (+ = headwind) | grade s16 0.01 % | Crr u8 0.0001 | Cw u8 0.01 kg/m
      if (n < 7) { respond(op, RES_INVALID_PARAM); break; }
      s.wind  = (int16_t)(d[1] | (d[2] << 8)) * 0.001f;
      s.grade = (int16_t)(d[3] | (d[4] << 8)) * 0.01f;
      s.crr   = d[5] * 0.0001f;
      s.cw    = d[6] * 0.01f;
      s.mode  = Mode::Sim;
      sim.logf("SIM grade %.2f%% wind %.2f m/s crr %.4f cw %.2f", s.grade, s.wind, s.crr, s.cw);
      respond(op, RES_SUCCESS);
      const uint8_t m[7] = { MS_SIM_PARAMS, d[1], d[2], d[3], d[4], d[5], d[6] };
      machineStatus(m, 7);
      break;
    }
  }
}

// ---- NimBLE callbacks -------------------------------------------------------
class ServerCb : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* server, ble_gap_conn_desc* desc) override {
    {
      SimLock l;
      sim.resetSession();
      sim.st.connected = true;
      sim.st.subscribed = false;
      clearQueue();
      sim.logf("BLE connected: %s", NimBLEAddress(desc->peer_ota_addr).toString().c_str());
    }
    server->updateConnParams(desc->conn_handle, 24, 48, 0, 180);
  }
  void onDisconnect(NimBLEServer*) override {
    SimLock l;
    sim.st.connected = false;
    sim.st.subscribed = false;
    sim.resetSession();
    clearQueue();
    sim.logf("BLE disconnected, advertising again");
  }
};

class CharCb : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* c) override {
    if (c != chCp) return;
    auto v = c->getValue();
    handleControlPoint(reinterpret_cast<const uint8_t*>(v.data()), v.size());
  }
  void onSubscribe(NimBLECharacteristic* c, ble_gap_conn_desc*, uint16_t subValue) override {
    SimLock l;
    const char* what = c == chBike ? "Indoor Bike Data" : c == chCp ? "Control Point" :
                       c == chStatus ? "Machine Status" : "Training Status";
    if (c == chBike) sim.st.subscribed = (subValue & 1) != 0;
    sim.logf("%s: %s", what, subValue ? "subscribed" : "unsubscribed");
  }
};

ServerCb serverCb;
CharCb charCb;

}  // namespace

// ---- Public API -------------------------------------------------------------
void ftmsBegin() {
  NimBLEDevice::init(BLE_DEVICE_NAME);
  NimBLEDevice::setPower(ESP_PWR_LVL_P9);

  gServer = NimBLEDevice::createServer();
  gServer->setCallbacks(&serverCb);
  gServer->advertiseOnDisconnect(true);

  NimBLEService* svc = gServer->createService(NimBLEUUID(UUID_FTMS));

  // Fitness Machine Feature: machine features u32 | target setting features u32
  //   machine: cadence(1) total distance(2) elapsed time(12) power measurement(14) = 0x00005006
  //   target : resistance(2) power(3) indoor bike simulation(13)                    = 0x0000200C
  const uint8_t feature[8] = { 0x06, 0x50, 0x00, 0x00, 0x0C, 0x20, 0x00, 0x00 };
  svc->createCharacteristic(NimBLEUUID(UUID_FEATURE), NIMBLE_PROPERTY::READ)->setValue(feature, sizeof(feature));

  chBike     = svc->createCharacteristic(NimBLEUUID(UUID_INDOOR_BIKE), NIMBLE_PROPERTY::NOTIFY);
  chTraining = svc->createCharacteristic(NimBLEUUID(UUID_TRAINING_STATUS), NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);

  // Supported ranges: min s16 | max s16 | increment u16
  uint8_t res[6], pwr[6];
  put16(res, 0); put16(res + 2, RESISTANCE_MAX); put16(res + 4, 1);
  put16(pwr, POWER_RANGE_MIN); put16(pwr + 2, POWER_RANGE_MAX); put16(pwr + 4, 1);
  svc->createCharacteristic(NimBLEUUID(UUID_RESISTANCE_RNG), NIMBLE_PROPERTY::READ)->setValue(res, sizeof(res));
  svc->createCharacteristic(NimBLEUUID(UUID_POWER_RNG), NIMBLE_PROPERTY::READ)->setValue(pwr, sizeof(pwr));

  chCp     = svc->createCharacteristic(NimBLEUUID(UUID_CONTROL_POINT), NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::INDICATE);
  chStatus = svc->createCharacteristic(NimBLEUUID(UUID_MACHINE_STATUS), NIMBLE_PROPERTY::NOTIFY);

  const uint8_t idle[2] = { 0x00, TS_IDLE };
  chTraining->setValue(idle, sizeof(idle));
  chBike->setCallbacks(&charCb);
  chTraining->setCallbacks(&charCb);
  chCp->setCallbacks(&charCb);
  chStatus->setCallbacks(&charCb);

  svc->start();

  // FTMS service data: flags (machine available) | machine type u16 (bit 5 = indoor bike)
  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  adv->addServiceUUID(NimBLEUUID(UUID_FTMS));
  adv->setServiceData(NimBLEUUID(UUID_FTMS), std::string("\x01\x20\x00", 3));
  adv->setScanResponse(true);
  adv->setMinInterval(32);   // 20 ms
  adv->setMaxInterval(64);   // 40 ms
  adv->start();

  sim.logf("BLE advertising as \"%s\"", BLE_DEVICE_NAME);
}

void ftmsLoop() {
  drainQueue();

  static uint32_t lastNotify = 0;
  const uint32_t now = millis();
  uint8_t pkt[16];
  size_t n = 0;
  {
    SimLock l;
    if (sim.st.connected && sim.st.subscribed && now - lastNotify >= (uint32_t)sim.cfg.notifyMs) {
      lastNotify = now;
      const bool stalled = sim.st.stallUntilMs && (int32_t)(now - sim.st.stallUntilMs) < 0;
      if (!stalled) n = buildBikeData(pkt);
    }
  }
  if (n) {
    chBike->setValue(pkt, n);
    chBike->notify();
  }
}

void ftmsDisconnect() {
  if (!gServer) return;
  for (uint16_t handle : gServer->getPeerDevices()) gServer->disconnect(handle);
}

bool ftmsInject(const String& kind, String& msg) {
  uint8_t pkt[16];
  size_t n = 0;
  {
    SimLock l;
    if (!sim.st.connected || !sim.st.subscribed) { msg = "no app is subscribed to Indoor Bike Data"; return false; }
    n = buildBikeData(pkt);   // valid packet to start from; offsets: speed 2, cadence 4, distance 6, power 9, elapsed 11
    if (kind == "short") {                 // truncated: flags + 1 byte
      n = 3;
    } else if (kind == "garbage") {        // random bytes, right length
      for (size_t i = 0; i < n; i++) pkt[i] = esp_random() & 0xFF;
    } else if (kind == "zero") {           // valid flags, every value zero
      memset(pkt + 2, 0, n - 2);
    } else if (kind == "max") {            // every value at its maximum
      memset(pkt + 2, 0xFF, n - 2);
      pkt[10] = 0x7F;                      // power = 32767 W (0xFFFF would be -1)
    } else if (kind == "negative") {       // power = -100 W
      put16(pkt + 9, (uint16_t)(int16_t)-100);
    } else {
      msg = "unknown packet kind (short, garbage, zero, max, negative)";
      return false;
    }
    sim.logf("injected '%s' Indoor Bike Data packet", kind.c_str());
  }
  chBike->setValue(pkt, n);
  chBike->notify();
  msg = String("sent ") + kind + " packet";
  return true;
}
