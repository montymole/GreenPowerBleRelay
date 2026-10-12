#pragma once

#include <Arduino.h>

void statusDisplayInit();
void statusDisplayUpdate(bool wifiConnected, bool wifiConnecting,
                         const String &ssid, const String &ipAddress, bool bleConnected,
                         bool bleReady, bool bleConnecting, bool active, bool hasError);
