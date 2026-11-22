#pragma once
#include <Arduino.h>

void ftmsBegin();                                   // start BLE + advertising
void ftmsLoop();                                    // send queued responses and periodic data
void ftmsDisconnect();                              // drop the connected app
bool ftmsInject(const String& kind, String& msg);   // send a deliberately odd data packet
