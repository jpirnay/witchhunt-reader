# Clock and sensors in fixed point

Part of [Integer maths](../integer-math.md). Area 5. Counts from `scripts/float_inventory.py` on 2026-11-21, after area 1.

**Open.** The clock's drift and temperature correction, the tilt and orientation tracking, and two SDK drivers use
float: about 110 call sites in 13 functions.

**Where.** `HalClock.cpp` (67: `syncNtp`, `restore`, `computeCorrectedElapsedSec`, `updatePeriodic`,
`rtcStateLooksSane`, `readChipTemperatureC`), `OrientationTracker.cpp` (12), `HalTiltSensor.cpp` (7),
`SerialTestApi.cpp` (6), `SyncTimeActivity.cpp` (4), and in the SDK `Imu.cpp` (12: `read`) and `BatteryMonitor.cpp`
(3: `readMillivolts`).

**Boundary.** ESP-IDF's `temperature_sensor_get_celsius` returns a float: convert it where `readChipTemperatureC`
reads it (or read the raw value). The IDF keeps its own float code.

**Next step.** Drift in parts per billion and temperatures in centi-degrees as integers; the IMU and battery changes
land on the SDK fork's `witchhunt` branch before a pin moves (`scripts/check_sdk_pin.sh`). The `clock_authority` and
`imu_motion` suites cover parts of it.
