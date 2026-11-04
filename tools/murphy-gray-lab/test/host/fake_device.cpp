// The lab firmware on a PC, with a fake UC8253 behind the bit-banged bus. test_lab.py talks to it
// over stdin and stdout exactly as it would talk to the board, and reads the bus log it writes.
//
// FAKE_BUS_LOG   file for one line per register write: "<cmd> <bytes> <first> <uniform>"
// FAKE_OTP       file to answer 0xA2 with
// FAKE_STUCK     BUSY never releases after a refresh
// FAKE_READ_MODE the one read handshake the fake answers: 0, 1 or 2 (default all)

#include <Arduino.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <deque>
#include <string>
#include <vector>

#include "Uc8253Lab.h"

void setup();
void loop();

namespace {

uint32_t now = 0;
FILE* busLog = nullptr;
std::vector<uint8_t> otpImage;
bool stuck = false;

// Bus state
int levels[64] = {};
int modes[64] = {};
uint8_t shift = 0;
uint8_t bitCount = 0;
int currentCommand = -1;
std::vector<uint8_t> payload;
bool refreshed = false;

// Read state
std::vector<uint8_t> readBytes;
size_t readBit = 0;

void flushPayload() {
  if (currentCommand < 0 || !busLog) return;
  bool same = true;
  for (uint8_t b : payload) same = same && b == payload.front();
  fprintf(busLog, "%02x %zu %02x %d\n", currentCommand, payload.size(), payload.empty() ? 0 : payload.front(), same);
  fflush(busLog);
}

void startCommand(uint8_t cmd) {
  flushPayload();
  currentCommand = cmd;
  payload.clear();
  readBytes.clear();
  readBit = 0;
  if (cmd == 0x12) refreshed = true;
  if (cmd == 0x70) readBytes = {0xFF, 0x09, 0xAA, 0xBB, 0xCC};
  if (cmd == 0x71) readBytes = {0x13};
  if (cmd == 0xA2) {
    readBytes.push_back(0x00);  // dummy
    readBytes.insert(readBytes.end(), otpImage.begin(), otpImage.end());
  }
}

void clockIn() {
  if (levels[lab::kPinCs] != LOW || modes[lab::kPinMosi] != OUTPUT) return;
  shift = static_cast<uint8_t>(shift << 1 | (levels[lab::kPinMosi] == HIGH));
  if (++bitCount < 8) return;
  bitCount = 0;
  if (levels[lab::kPinDc] == LOW) {
    startCommand(shift);
  } else {
    payload.push_back(shift);
  }
}

// Only what real hardware cannot be asked to do twice gets a hard stop here.
bool stdinOpen = true;
std::deque<uint8_t> input;

void fill() {
  uint8_t buffer[4096];
  const ssize_t got = ::read(0, buffer, sizeof(buffer));
  if (got <= 0) {
    stdinOpen = false;
    return;
  }
  input.insert(input.end(), buffer, buffer + got);
}

}  // namespace

void pinMode(uint8_t pin, int mode) { modes[pin] = mode == INPUT_PULLUP ? INPUT : mode; }

void digitalWrite(uint8_t pin, int value) {
  const int before = levels[pin];
  levels[pin] = value;
  if (pin != lab::kPinClock) return;
  if (before == LOW && value == HIGH) clockIn();
  // The controller shifts out on the falling edge.
  if (before == HIGH && value == LOW && modes[lab::kPinMosi] == INPUT) readBit++;
}

int digitalRead(uint8_t pin) {
  if (pin == lab::kPinBusy) return stuck && refreshed ? LOW : HIGH;
  if (pin == lab::kPinMosi && readBit / 8 < readBytes.size()) {
    return (readBytes[readBit / 8] >> (7 - readBit % 8)) & 1;
  }
  return HIGH;  // a released line floats up, like an unanswered read
}

void delay(uint32_t ms) { now += ms; }
void delayMicroseconds(uint32_t) {}
uint32_t millis() { return now++; }

FakeSerial Serial;

int FakeSerial::available() {
  if (input.empty() && stdinOpen) fill();
  return static_cast<int>(input.size());
}

int FakeSerial::read() {
  if (!available()) return -1;
  const int c = input.front();
  input.pop_front();
  return c;
}

size_t FakeSerial::readBytes(char* out, size_t length) {
  size_t done = 0;
  while (done < length && available()) out[done++] = static_cast<char>(read());
  return done;
}

void FakeSerial::print(const char* text) { fputs(text, stdout); }
void FakeSerial::print(char c) { fputc(c, stdout); }
void FakeSerial::print(int value) { printf("%d", value); }
void FakeSerial::print(uint8_t value) { printf("%u", value); }
void FakeSerial::print(uint32_t value) { printf("%u", value); }
void FakeSerial::println() {
  fputc('\n', stdout);
  fflush(stdout);
}
void FakeSerial::println(const char* text) {
  fputs(text, stdout);
  println();
}
void FakeSerial::println(int value) {
  print(value);
  println();
}
void FakeSerial::println(uint32_t value) {
  print(value);
  println();
}

int main() {
  if (const char* path = getenv("FAKE_BUS_LOG")) busLog = fopen(path, "w");
  stuck = getenv("FAKE_STUCK") != nullptr;
  otpImage.assign(8192, 0xFF);
  if (const char* path = getenv("FAKE_OTP")) {
    if (FILE* file = fopen(path, "rb")) {
      fread(otpImage.data(), 1, otpImage.size(), file);
      fclose(file);
    }
  }
  setup();
  while (stdinOpen || !input.empty()) loop();
  flushPayload();
  return 0;
}
