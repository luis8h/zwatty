#include "control.h"
#include <stdarg.h>
#include <stdlib.h>
#include "ftms.h"

const SettingDef SETTING_DEFS[] = {
  // key             label                              unit   member                    min  max   step
  {"pedalling",     "Rider is pedalling",              "",    &Settings::pedalling,     0,   1,    1},
  {"cadence",       "Cadence",                         "rpm", &Settings::cadence,       0,   150,  1},
  {"cadence_noise", "Cadence noise (std dev)",         "rpm", &Settings::cadenceNoise,  0,   20,   0.5f},
  {"rider_power",   "Rider power (all modes but ERG)", "W",   &Settings::riderPower,    0,   1500, 5},
  {"power_noise",   "Power noise (std dev)",           "%",   &Settings::powerNoise,    0,   30,   0.5f},
  {"power_drift",   "Power drift",                     "%",   &Settings::powerDrift,    0,   30,   0.5f},
  {"erg_lag",       "ERG response lag",                "s",   &Settings::ergLag,        0,   30,   0.5f},
  {"max_power",     "Rider max power (ERG cap)",       "W",   &Settings::maxPower,      0,   2000, 10},
  {"rider_mass",    "Rider + bike mass",               "kg",  &Settings::riderMass,     40,  200,  1},
  {"notify_ms",     "Data notify interval",            "ms",  &Settings::notifyMs,      100, 5000, 50},
  {"ctrl_delay_ms", "Control point response delay",    "ms",  &Settings::ctrlDelayMs,   0,   10000, 100},
  {"ctrl_silent",   "Never answer control point",      "",    &Settings::ctrlSilent,    0,   1,    1},
  {"fail_next",     "Fail the next N commands",        "",    &Settings::failNext,      0,   20,   1},
  {"fail_code",     "Failure result code (2-5)",       "",    &Settings::failCode,      2,   5,    1},
};
const size_t SETTING_COUNT = sizeof(SETTING_DEFS) / sizeof(SETTING_DEFS[0]);

// ---- helpers ----------------------------------------------------------------
static void jf(String& s, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
static void jf(String& s, const char* fmt, ...) {
  char b[160];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(b, sizeof(b), fmt, ap);
  va_end(ap);
  s += b;
}

String jsonEscape(const String& s) {
  String o;
  o.reserve(s.length() + 4);
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '"' || c == '\\') { o += '\\'; o += c; }
    else if ((uint8_t)c < 0x20) o += ' ';
    else o += c;
  }
  return o;
}

static const char* tf(bool b) { return b ? "true" : "false"; }

// ---- settings ---------------------------------------------------------------
bool applySetting(const String& key, const String& rawValue, String& err) {
  String value = rawValue;
  value.trim();
  for (size_t i = 0; i < SETTING_COUNT; i++) {
    const SettingDef& d = SETTING_DEFS[i];
    if (!key.equalsIgnoreCase(d.key)) continue;
    char* end = nullptr;
    float v = strtof(value.c_str(), &end);
    if (value.length() == 0 || *end != '\0') { err = String(d.key) + ": not a number: '" + value + "'"; return false; }
    if (v < d.min || v > d.max) {
      char b[96];
      snprintf(b, sizeof(b), "%s: out of range (%g to %g)", d.key, d.min, d.max);
      err = b;
      return false;
    }
    SimLock l;
    sim.cfg.*(d.ptr) = v;
    return true;
  }
  err = String("unknown setting: ") + key;
  return false;
}

// ---- actions ----------------------------------------------------------------
bool runAction(const String& name, const String& arg, String& msg) {
  if (name == "drop") {
    ftmsDisconnect();
    msg = "disconnect requested";
    return true;
  }
  if (name == "stall") {
    float secs = arg.toFloat();
    if (secs <= 0 || secs > 600) { msg = "stall needs seconds between 0 and 600"; return false; }
    SimLock l;
    sim.st.stallUntilMs = millis() + (uint32_t)(secs * 1000);
    sim.logf("data notifications stalled for %.0f s", secs);
    msg = "stalled";
    return true;
  }
  if (name == "inject") return ftmsInject(arg, msg);
  if (name == "reset") {
    sim.resetSession();
    sim.logf("session reset (control, mode, targets, distance)");
    msg = "session reset";
    return true;
  }
  if (name == "defaults") {
    SimLock l;
    sim.cfg = Settings();
    sim.logf("settings restored to defaults");
    msg = "defaults restored";
    return true;
  }
  msg = String("unknown action: ") + name;
  return false;
}

static bool isAction(const String& n) {
  return n == "drop" || n == "stall" || n == "inject" || n == "reset" || n == "defaults";
}

// ---- JSON -------------------------------------------------------------------
String stateJson() {
  SimLock l;
  const SimState& s = sim.st;
  const uint32_t now = millis();
  const bool stalled = s.stallUntilMs && (int32_t)(now - s.stallUntilMs) < 0;

  String j;
  j.reserve(2600);
  j += '{';
  jf(j, "\"connected\":%s,\"subscribed\":%s,\"control\":%s,\"running\":%s,\"stalled\":%s,",
     tf(s.connected), tf(s.subscribed), tf(s.haveControl), tf(s.running), tf(stalled));
  jf(j, "\"mode\":\"%s\",\"target_power\":%d,\"resistance\":%u,", modeName(s.mode), (int)s.targetPower, (unsigned)s.resistance);
  jf(j, "\"grade\":%.2f,\"wind\":%.2f,\"crr\":%.4f,\"cw\":%.2f,", s.grade, s.wind, s.crr, s.cw);
  jf(j, "\"power\":%.0f,\"cadence\":%.1f,\"speed_kmh\":%.1f,\"distance_m\":%.0f,\"elapsed_s\":%.0f,",
     s.powerOut, s.cadenceOut, s.speed * 3.6f, s.distance, s.elapsed);
  jf(j, "\"uptime_s\":%lu,\"cfg\":{", (unsigned long)(now / 1000));
  for (size_t i = 0; i < SETTING_COUNT; i++) {
    jf(j, "%s\"%s\":%g", i ? "," : "", SETTING_DEFS[i].key, sim.cfg.*(SETTING_DEFS[i].ptr));
  }
  j += "},\"log\":[";
  const uint32_t count = sim.logCount < LOG_LINES ? sim.logCount : LOG_LINES;
  const uint32_t first = sim.logCount - count;
  for (uint32_t i = 0; i < count; i++) {
    const LogEntry& e = sim.logbuf[(first + i) % LOG_LINES];
    jf(j, "%s{\"t\":%lu,\"m\":\"", i ? "," : "", (unsigned long)e.t);
    j += jsonEscape(String(e.msg));
    j += "\"}";
  }
  j += "]}";
  return j;
}

String metaJson() {
  String j = "[";
  for (size_t i = 0; i < SETTING_COUNT; i++) {
    const SettingDef& d = SETTING_DEFS[i];
    jf(j, "%s{\"key\":\"%s\",\"label\":\"%s\",\"unit\":\"%s\",\"min\":%g,\"max\":%g,\"step\":%g}",
       i ? "," : "", d.key, d.label, d.unit, d.min, d.max, d.step);
  }
  j += ']';
  return j;
}

// ---- serial console ---------------------------------------------------------
static String helpText() {
  String h =
    "Commands:\n"
    "  <setting> <value>      e.g. 'cadence 90'  (also: 'set cadence 90')\n"
    "  settings               list settings and current values\n"
    "  state                  print state as JSON\n"
    "  drop                   disconnect the app\n"
    "  stall <seconds>        stop sending data for N seconds\n"
    "  inject <kind>          short | garbage | zero | max | negative\n"
    "  reset                  reset session (control, mode, targets, distance)\n"
    "  defaults               restore default settings\n"
    "Settings:";
  for (size_t i = 0; i < SETTING_COUNT; i++) {
    h += "\n  ";
    h += SETTING_DEFS[i].key;
  }
  return h;
}

String execLine(String line) {
  line.trim();
  if (line.length() == 0) return "";
  int sp = line.indexOf(' ');
  String cmd = sp < 0 ? line : line.substring(0, sp);
  String rest = sp < 0 ? String("") : line.substring(sp + 1);
  rest.trim();
  cmd.toLowerCase();

  if (cmd == "help" || cmd == "?") return helpText();
  if (cmd == "state") return stateJson();
  if (cmd == "set") return execLine(rest);
  if (cmd == "settings") {
    SimLock l;
    String o;
    for (size_t i = 0; i < SETTING_COUNT; i++) {
      const SettingDef& d = SETTING_DEFS[i];
      char b[110];
      snprintf(b, sizeof(b), "%s%-14s = %-8g %-4s (%g to %g)", i ? "\n" : "", d.key, sim.cfg.*(d.ptr), d.unit, d.min, d.max);
      o += b;
    }
    return o;
  }

  String out;
  if (isAction(cmd)) return (runAction(cmd, rest, out) ? String("ok: ") : String("error: ")) + out;
  if (applySetting(cmd, rest, out)) return "ok";
  return String("error: ") + out + "  (type 'help')";
}
