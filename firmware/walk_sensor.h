#pragma once

#include <Arduino.h>

// QMI8658 pedometer used only while the walk screen is active. The gyroscope
// stays disabled to reduce current draw; the accelerometer runs at 62.5 Hz.
bool walkSensorStart();
// Restores a walk after an ESP restart. savedRaw is the QMI counter captured
// with savedTotal; any autonomous steps recorded since then are retained.
bool walkSensorRestore(uint32_t savedTotal, uint32_t savedRaw);
// Samples acceleration frequently enough for the orientation-independent
// software fallback detector. Call from every game loop while walking.
void walkSensorUpdate(uint32_t nowMs);
uint32_t walkSensorSteps();
uint32_t walkSensorRawSteps();
// Re-anchor the autonomous hardware counter around an ESP light-sleep period.
// This preserves software-detected steps already earned while adding every
// QMI8658 step recorded while the main loop is suspended.
void walkSensorPrepareSleep();
void walkSensorResumeFromSleep();
void walkSensorStop();
bool walkSensorActive();
