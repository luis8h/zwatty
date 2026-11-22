#include "console.h"
#include <Arduino.h>
#include "control.h"

static String lineBuf;

void consoleBegin() {
  Serial.println("Serial console ready. Type 'help'.");
}

void consoleLoop() {
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      if (lineBuf.length()) {
        String out = execLine(lineBuf);
        lineBuf = "";
        if (out.length()) Serial.println(out);
      }
    } else if (lineBuf.length() < 120) {
      lineBuf += c;
    }
  }
}
