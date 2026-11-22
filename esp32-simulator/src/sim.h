#pragma once
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

// What the connected app last asked the "trainer" to do.
enum class Mode : uint8_t { Free, Resistance, Erg, Sim };
const char* modeName(Mode m);

// User-tunable behaviour. Everything is a float so one table (control.cpp)
// can drive the web UI, the HTTP API and the serial console.
struct Settings {
  float pedalling    = 1;     // 0/1: is the virtual rider pedalling
  float cadence      = 85;    // rpm
  float cadenceNoise = 2;     // rpm, std dev
  float riderPower   = 180;   // W, used in every mode except ERG
  float powerNoise   = 2;     // % of power, std dev
  float powerDrift   = 0;     // % of power, slow oscillation
  float ergLag       = 2;     // s, time constant towards ERG target
  float maxPower     = 1000;  // W, rider cannot exceed this in ERG
  float riderMass    = 85;    // kg, rider + bike (speed model)
  float notifyMs     = 1000;  // Indoor Bike Data interval
  float ctrlDelayMs  = 0;     // delay before control point responses
  float ctrlSilent   = 0;     // 0/1: never answer control point writes
  float failNext     = 0;     // next N control point writes fail
  float failCode     = 4;     // result code used for failures (2..5)
};

struct SimState {
  // link / FTMS state
  bool connected  = false;
  bool subscribed = false;    // app enabled Indoor Bike Data notifications
  bool haveControl = false;   // app sent Request Control
  bool running    = false;    // Start/Resume received
  Mode mode       = Mode::Free;
  int16_t targetPower = 0;    // W (ERG)
  uint8_t resistance  = 0;    // 0.1 units
  float grade = 0;            // %
  float wind  = 0;            // m/s, positive = headwind
  float crr   = 0.004f;
  float cw    = 0.51f;        // kg/m (0.5 * rho * CdA)
  // simulated output
  float cadence = 0, cadenceOut = 0;  // rpm (smoothed / with noise)
  float power = 0, powerOut = 0;      // W   (smoothed / with noise+drift)
  float speed = 0;                    // m/s
  float distance = 0;                 // m
  float elapsed = 0;                  // s (counts while running)
  uint32_t stallUntilMs = 0;          // no data notifications until then
};

constexpr size_t LOG_LINES = 24;
struct LogEntry { uint32_t t; char msg[80]; };

class Sim {
 public:
  Settings cfg;
  SimState st;
  LogEntry logbuf[LOG_LINES] = {};
  uint32_t logCount = 0;

  void begin();
  void tick();                 // call often; advances physics in 50 ms steps
  void resetSession();         // clears app-controlled state, keeps settings
  void logf(const char* fmt, ...) __attribute__((format(printf, 2, 3)));

  // Fields above are shared between the loop task and the BLE host task.
  // Hold the lock (SimLock) while touching them. The mutex is recursive.
  void lock()   { xSemaphoreTakeRecursive(mtx_, portMAX_DELAY); }
  void unlock() { xSemaphoreGiveRecursive(mtx_); }

 private:
  SemaphoreHandle_t mtx_ = nullptr;
  uint32_t lastTick_ = 0;
  float driftPhase_ = 0;
  float solveSpeed(float watts) const;
};

extern Sim sim;

struct SimLock {
  SimLock()  { sim.lock(); }
  ~SimLock() { sim.unlock(); }
};
