#include "../src/connection_state.h"
#include <cassert>
#include <cstdio>

int main() {
  // Missing AP at boot: do not repeatedly interrupt an in-flight association.
  WifiRetryState wifi;
  assert(wifi.update(100, false) == WifiAction::Begin);
  for (uint32_t t = 150; t < 30100; t += 50) {
    assert(wifi.update(t, false) == WifiAction::None);
    assert(wifi.connecting());
  }
  assert(wifi.update(30100, false) == WifiAction::Disconnect);
  assert(!wifi.connecting());
  assert(wifi.update(35099, false) == WifiAction::None);
  assert(wifi.update(35100, false) == WifiAction::Begin);
  assert(wifi.update(36000, true) == WifiAction::None);
  assert(!wifi.connecting());
  // Repeated outages recover without a reboot, including late connection success.
  for (uint32_t t = 40000; t < 100000; t += 10000) {
    assert(wifi.update(t, false) == WifiAction::Reconnect);
    assert(wifi.update(t + 50, false) == WifiAction::None);
    assert(wifi.update(t + 500, true) == WifiAction::None);
    assert(!wifi.connecting());
  }
  // millis() wraparound must not stall retries.
  WifiRetryState wrap;
  const uint32_t start = UINT32_MAX - 1000;
  assert(wrap.update(start, false) == WifiAction::Begin);
  assert(wrap.update(start + 29999u, false) == WifiAction::None);
  assert(wrap.update(start + 30000u, false) == WifiAction::Disconnect);
  assert(wrap.update(start + 35000u, false) == WifiAction::Begin);

  // Both arrival orders: pending -> one connected -> both ready.
  assert(relayStatus(false, true, false, false, true, false, false) == RelayStatus::Working);
  assert(relayStatus(true, false, false, false, true, false, false) == RelayStatus::Working);
  assert(relayStatus(false, true, true, true, false, false, false) == RelayStatus::Working);
  assert(relayStatus(true, false, true, true, false, false, false) == RelayStatus::Ready);
  // Error -> retry/activity -> recovered; neither error nor blue remains latched.
  assert(relayStatus(false, false, true, true, false, false, false) == RelayStatus::Error);
  assert(relayStatus(true, false, false, false, false, false, true) == RelayStatus::Error);
  assert(relayStatus(true, false, false, false, true, false, true) == RelayStatus::Working);
  assert(relayStatus(true, false, true, true, false, false, false) == RelayStatus::Ready);
  assert(relayStatus(true, false, true, true, false, false, true) == RelayStatus::Error);
  assert(relayStatus(true, false, true, true, false, true, true) == RelayStatus::Working);
  assert(relayStatus(true, false, true, true, false, false, false) == RelayStatus::Ready);
  puts("Connection retry and relay status scenarios passed");
}
