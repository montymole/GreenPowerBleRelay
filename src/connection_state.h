#pragma once

#include <stdint.h>

enum class WifiAction { None, Begin, Reconnect, Disconnect };

class WifiRetryState {
 public:
  WifiAction update(uint32_t now, bool connected, uint8_t networkCount = 1) {
    if (!networkCount) { connecting_ = false; return WifiAction::None; }
    if (connected) {
      started_ = true;
      wasConnected_ = true;
      connecting_ = false;
      return WifiAction::None;
    }
    if (!started_ || wasConnected_) {
      const auto action = wasConnected_ ? WifiAction::Reconnect : WifiAction::Begin;
      started_ = true;
      wasConnected_ = false;
      connecting_ = true;
      phaseStarted_ = now;
      return action;
    }
    if (connecting_ && uint32_t(now - phaseStarted_) >= 30000) {
      networkIndex_ = (networkIndex_ + 1) % networkCount;
      connecting_ = false;
      phaseStarted_ = now;
      return WifiAction::Disconnect;
    }
    if (!connecting_ && uint32_t(now - phaseStarted_) >= 5000) {
      connecting_ = true;
      phaseStarted_ = now;
      return WifiAction::Begin;
    }
    return WifiAction::None;
  }

  bool connecting() const { return connecting_; }
  uint8_t networkIndex() const { return networkIndex_; }
  void reset(uint8_t index = 0) { *this = WifiRetryState(); networkIndex_ = index; }

 private:
  bool started_ = false;
  bool wasConnected_ = false;
  bool connecting_ = false;
  uint32_t phaseStarted_ = 0;
  uint8_t networkIndex_ = 0;
};

enum class RelayStatus { Working, Ready, Error };

inline RelayStatus relayStatus(bool wifiConnected, bool wifiConnecting,
                               bool bleConnected, bool bleReady, bool bleConnecting,
                               bool active, bool hasError) {
  if (wifiConnecting || bleConnecting || active) return RelayStatus::Working;
  if (wifiConnected && bleConnected && bleReady && !hasError) return RelayStatus::Ready;
  return RelayStatus::Error;
}
