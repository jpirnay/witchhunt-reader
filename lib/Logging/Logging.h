#ifndef LOGGING_H
#define LOGGING_H

#include <HardwareSerial.h>

// Define ENABLE_SERIAL_LOG to enable logging
// Can be set in platformio.ini build_flags or as a compile definition

#ifdef ENABLE_SERIAL_LOG
#define LOG(origin, format, ...) Serial.printf("[%lu] [%s] " format "\n", millis(), origin, ##__VA_ARGS__)
#else
#define LOG(origin, format, ...)
#endif

#endif // LOGGING_H