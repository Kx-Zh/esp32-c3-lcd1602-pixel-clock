#pragma once

#include <Arduino.h>
#include <Wire.h>

// Minimal write-only HD44780 4-bit driver for the common PCF8574T backpack.
// It deliberately avoids address and pin-map auto-detection so the electrical
// mapping is explicit and easy to diagnose.
class Pcf8574tLcd {
 public:
  Pcf8574tLcd(uint8_t address, uint8_t rsMask,
              uint8_t enableMask, uint8_t backlightMask)
      : address_(address),
        rsMask_(rsMask),
        enableMask_(enableMask),
        backlightMask_(backlightMask) {}

  int begin(uint8_t columns, uint8_t rows) {
    columns_ = columns;
    rows_ = rows;
    backlightOn_ = true;

    delay(50);  // HD44780 power-on settling time.
    if (!writeExpander(outputBase())) return -4;

    // HD44780 reset sequence when the controller state is unknown.
    writeInitNibble(0x03);
    delayMicroseconds(4500);
    writeInitNibble(0x03);
    delayMicroseconds(4500);
    writeInitNibble(0x03);
    delayMicroseconds(150);
    writeInitNibble(0x02);  // Enter 4-bit mode.

    command(0x28);  // 4-bit, 2 lines, 5x8 font.
    command(0x08);  // Display off while configuring.
    clear();
    command(0x06);  // Cursor moves right after each byte.
    command(0x0C);  // Display on, cursor off, blink off.
    return 0;
  }

  void clear() {
    command(0x01);
    delayMicroseconds(2000);
  }

  void display() {
    command(0x0C);
  }

  void backlight() {
    backlightOn_ = true;
    writeExpander(outputBase());
  }

  void noBacklight() {
    backlightOn_ = false;
    writeExpander(outputBase());
  }

  void setCursor(uint8_t column, uint8_t row) {
    static constexpr uint8_t ROW_OFFSETS[] = {0x00, 0x40, 0x14, 0x54};
    if (rows_ == 0) return;
    if (row >= rows_) row = rows_ - 1;
    if (column >= columns_) column = columns_ - 1;
    command(static_cast<uint8_t>(0x80 | (column + ROW_OFFSETS[row])));
  }

  void createChar(uint8_t location, const uint8_t charmap[]) {
    location &= 0x07;
    command(static_cast<uint8_t>(0x40 | (location << 3)));
    for (uint8_t row = 0; row < 8; ++row) write(charmap[row]);
  }

  size_t write(uint8_t value) {
    send(value, true);
    return 1;
  }

 private:
  uint8_t outputBase() const {
    // P1/RW is intentionally omitted, keeping it low for write-only operation.
    return backlightOn_ ? backlightMask_ : 0;
  }

  bool writeExpander(uint8_t value) {
    Wire.beginTransmission(address_);
    Wire.write(value);
    return Wire.endTransmission() == 0;
  }

  void pulseEnable(uint8_t value) {
    writeExpander(static_cast<uint8_t>(value | enableMask_));
    delayMicroseconds(1);
    writeExpander(static_cast<uint8_t>(value & ~enableMask_));
    delayMicroseconds(50);
  }

  void writeInitNibble(uint8_t nibble) {
    const uint8_t value = static_cast<uint8_t>((nibble << 4) | outputBase());
    writeExpander(value);
    pulseEnable(value);
  }

  void writeNibble(uint8_t nibble, bool dataMode) {
    uint8_t value = static_cast<uint8_t>((nibble << 4) | outputBase());
    if (dataMode) value |= rsMask_;
    writeExpander(value);
    pulseEnable(value);
  }

  void send(uint8_t value, bool dataMode) {
    writeNibble(static_cast<uint8_t>(value >> 4), dataMode);
    writeNibble(static_cast<uint8_t>(value & 0x0F), dataMode);
  }

  void command(uint8_t value) {
    send(value, false);
    if (value != 0x01 && value != 0x02) delayMicroseconds(50);
  }

  const uint8_t address_;
  const uint8_t rsMask_;
  const uint8_t enableMask_;
  const uint8_t backlightMask_;
  uint8_t columns_ = 16;
  uint8_t rows_ = 2;
  bool backlightOn_ = true;
};

