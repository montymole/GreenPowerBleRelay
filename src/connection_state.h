#pragma once

#include <stdint.h>

enum class WifiAction { None, Begin, Reconnect, Disconnect };

class WifiRetryState {
 public:
  WifiAction update(uint32_t now, bool connected) {
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

 private:
  bool started_ = false;
  bool wasConnected_ = false;
  bool connecting_ = false;
  uint32_t phaseStarted_ = 0;
};

enum class RelayStatus { Working, Ready, Error };

inline RelayStatus relayStatus(bool wifiConnected, bool wifiConnecting,
                               bool bleConnected, bool bleReady, bool bleConnecting,
                               bool active, bool hasError) {
  if (wifiConnecting || bleConnecting || active) return RelayStatus::Working;
  if (wifiConnected && bleConnected && bleReady && !hasError) return RelayStatus::Ready;
  return RelayStatus::Error;
}
