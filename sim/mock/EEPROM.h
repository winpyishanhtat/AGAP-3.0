#pragma once
#include "Arduino.h"

struct EEPROMClass {
  uint8_t read(int i) { return sim::eeprom()[i]; }
  void update(int i, uint8_t v) { sim::eeprom()[i] = v; }
};
static EEPROMClass EEPROM;
