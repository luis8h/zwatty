#include "sim.h"
#include <math.h>
#include <stdarg.h>
#include <esp_system.h>

Sim sim;

const char* modeName(Mode m) {
  switch (m) {
    case Mode::Erg:        return "erg";
    case Mode::Sim:        return "sim";
    case Mode::Resistance: return "resistance";
    default:               return "free";
  }
}

// Standard normal random number (Box-Muller) from the hardware RNG.
static float gauss() {
  float u1 = ((float)esp_random() + 1.0f) / 4294967296.0f;
  float u2 = (float)esp_random() / 4294967296.0f;
  return sqrtf(-2.0f * logf(u1)) * cosf(2.0f * PI * u2);
}

void Sim::begin() {
  mtx_ = xSemaphoreCreateRecursiveMutex();
  lastTick_ = millis();
}

void Sim::resetSession() {
  SimLock l;
  st.haveControl = false;
  st.running = false;
  st.mode = Mode::Free;
  st.targetPower = 0;
  st.resistance = 0;
  st.grade = 0;
  st.wind = 0;
  st.crr = 0.004f;
  st.cw = 0.51f;
  st.distance = 0;
  st.elapsed = 0;
  st.stallUntilMs = 0;
}

void Sim::logf(const char* fmt, ...) {
  SimLock l;
  LogEntry& e = logbuf[logCount % LOG_LINES];
  e.t = millis();
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(e.msg, sizeof(e.msg), fmt, ap);
  va_end(ap);
  logCount++;
  Serial.printf("[%lu.%03lu] %s\n", (unsigned long)(e.t / 1000), (unsigned long)(e.t % 1000), e.msg);
}

// Speed (m/s) at which the resistance forces balance `watts` at the wheel.
// Only SIM mode uses grade/wind/Crr/Cw from the app; other modes ride on the flat.
float Sim::solveSpeed(float watts) const {
  const float g = 9.80665f, efficiency = 0.975f;
  const bool sm = (st.mode == Mode::Sim);
  const float grade = sm ? st.grade : 0.0f;
  const float wind  = sm ? st.wind  : 0.0f;
  const float crr   = sm ? st.crr   : 0.004f;
  const float cw    = sm ? st.cw    : 0.51f;
  const float theta = atanf(grade / 100.0f);
  const float fixed = cfg.riderMass * g * (crr * cosf(theta) + sinf(theta));
  const float target = watts * efficiency;

  float lo = 0, hi = 40;   // m/s
  for (int i = 0; i < 30; i++) {
    float v = 0.5f * (lo + hi);
    float va = v + wind;
    float force = cw * va * fabsf(va) + fixed;
    if (force * v > target) hi = v; else lo = v;
  }
  return 0.5f * (lo + hi);
}

void Sim::tick() {
  uint32_t now = millis();
  if (now - lastTick_ < 50) return;
  float dt = (now - lastTick_) / 1000.0f;
  lastTick_ = now;
  if (dt > 0.5f) dt = 0.5f;

  SimLock l;
  const bool pedalling = cfg.pedalling >= 0.5f;

  // cadence: smooth towards the setpoint, add noise on the reported value
  st.cadence += ((pedalling ? cfg.cadence : 0.0f) - st.cadence) * (1.0f - expf(-dt / 1.0f));
  st.cadenceOut = (pedalling && st.cadence > 1.0f) ? fmaxf(0.0f, st.cadence + gauss() * cfg.cadenceNoise) : 0.0f;

  // power: ERG follows the target with a lag, other modes follow the rider
  float desired = 0;
  if (pedalling) desired = (st.mode == Mode::Erg) ? fminf((float)st.targetPower, cfg.maxPower) : cfg.riderPower;
  const float tau = (st.mode == Mode::Erg) ? cfg.ergLag : 0.3f;
  if (tau < 0.05f) st.power = desired;
  else st.power += (desired - st.power) * (1.0f - expf(-dt / tau));

  driftPhase_ += dt;
  float drift = (0.6f * sinf(driftPhase_ * 2.0f * PI / 60.0f) + 0.4f * sinf(driftPhase_ * 2.0f * PI / 17.0f))
                * (cfg.powerDrift / 100.0f) * st.power;
  float noise = gauss() * (cfg.powerNoise / 100.0f) * st.power;
  st.powerOut = (st.power < 0.5f) ? 0.0f : fminf(fmaxf(st.power + drift + noise, 0.0f), 2000.0f);

  // speed and distance
  st.speed += (solveSpeed(st.power) - st.speed) * (1.0f - expf(-dt / 3.0f));
  if (st.speed < 0.01f) st.speed = 0;
  st.distance += st.speed * dt;
  if (st.running) st.elapsed += dt;
}
