#pragma once

// Just enough Arduino for fake_device.cpp to run the lab firmware on a PC.

#include <stddef.h>
#include <stdint.h>

#define OUTPUT 1
#define INPUT 0
#define INPUT_PULLUP 2
#define HIGH 1
#define LOW 0

void pinMode(uint8_t pin, int mode);
void digitalWrite(uint8_t pin, int value);
int digitalRead(uint8_t pin);
void delay(uint32_t ms);
void delayMicroseconds(uint32_t us);
uint32_t millis();

class FakeSerial {
 public:
  void begin(unsigned long) {}
  void setTimeout(uint32_t) {}
  void setRxBufferSize(size_t) {}
  int available();
  int read();
  size_t readBytes(char* out, size_t length);
  void print(const char* text);
  void print(char c);
  void print(int value);
  void print(uint8_t value);
  void print(uint32_t value);
  void println();
  void println(const char* text);
  void println(int value);
  void println(uint32_t value);
};

extern FakeSerial Serial;
