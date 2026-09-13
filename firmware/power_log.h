#pragma once
#include <Arduino.h>

// Bounded diagnostics only; never used to make gameplay/power decisions.
#ifdef _WIN32
inline void powerLogRecord(const char *, const char *, bool, bool, bool, int, bool) {}
inline void powerLogSleepResult(uint64_t, int, int) {}
inline bool powerLogCommand(const String &) { return false; }
#else
void powerLogRecord(const char *event, const char *scene, bool screenOff,
                    bool walkRest, bool imuRequested, int displayedPercent,
                    bool usbCached);
void powerLogSleepResult(uint64_t elapsedUs, int error, int wakeCause);
bool powerLogCommand(const String &line);
#endif
