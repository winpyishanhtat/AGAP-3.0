#pragma once
#include "Arduino.h"

class Servo {
 public:
  uint8_t attach(int pin) {
    pin_ = pin;
    sim::servoAttached(pin) = true;
    return 1;
  }
  void write(int angle) {
    // Like the real library, a write to a servo that is not attached does nothing.
    if (pin_ < 0 || !sim::servoAttached(pin_)) return;
    sim::servoAngle()[pin_] = angle;
    sim::servoWrites().push_back({sim::now(), pin_, angle});
  }

 private:
  int pin_ = -1;
};
