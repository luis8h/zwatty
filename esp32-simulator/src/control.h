#pragma once
// Shared control surface used by the web UI, the HTTP API and the serial console.
#include <Arduino.h>
#include "sim.h"

struct SettingDef {
  const char* key; const char* label; const char* unit;
  float Settings::* ptr;
  float min, max, step;
};
extern const SettingDef SETTING_DEFS[];
extern const size_t SETTING_COUNT;

bool applySetting(const String& key, const String& value, String& err);
bool runAction(const String& name, const String& arg, String& msg);
String execLine(String line);      // one serial console line -> reply text
String stateJson();
String metaJson();
String jsonEscape(const String& s);
