#include <Arduino.h>
#include "config.h"
#include "sim.h"
#include "ftms.h"
#include "web.h"
#include "console.h"

void setup() {
  Serial.begin(SERIAL_BAUD);
  delay(200);
  Serial.println("\nFTMS trainer simulator");
  sim.begin();
  ftmsBegin();
  webBegin();
  consoleBegin();
}

void loop() {
  sim.tick();
  ftmsLoop();
  webLoop();
  consoleLoop();
  delay(5);
}
