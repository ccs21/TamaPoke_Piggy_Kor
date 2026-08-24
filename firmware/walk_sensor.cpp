#include "walk_sensor.h"

#include <Wire.h>
#include <SensorQMI8658.hpp>
#include <math.h>

namespace {
SensorQMI8658 qmi;
bool initialized = false;
bool active = false;
uint32_t softwareSteps = 0;
uint32_t hardwareSteps = 0;
uint32_t hardwareAnchorRaw = 0;
uint32_t hardwareAnchorTotal = 0;
uint32_t lastSampleAt = 0;
uint32_t lastStepAt = 0;
float gravityX = 0.0f;
float gravityY = 0.0f;
float gravityZ = 1.0f;
float filteredMotion = 0.0f;
float magnitudeBaseline = 1.0f;
float filteredImpact = 0.0f;
uint8_t calibrationSamples = 0;
bool peakLatched = false;

uint32_t mappedHardwareTotal(uint32_t raw) {
  // Walk rewards finish at 1000 steps, long before the QMI counter can wrap.
  // Treat a smaller value as a counter reset and continue from the new value
  // rather than producing an unsigned jump.
  const uint32_t delta = raw >= hardwareAnchorRaw ? raw - hardwareAnchorRaw : raw;
  const uint64_t total = (uint64_t)hardwareAnchorTotal + delta;
  return total > 0xFFFFFFUL ? 0xFFFFFFUL : (uint32_t)total;
}

void resetSoftwareDetector(uint32_t total) {
  softwareSteps = total;
  lastSampleAt = 0;
  lastStepAt = 0;
  gravityX = 0.0f;
  gravityY = 0.0f;
  gravityZ = 1.0f;
  filteredMotion = 0.0f;
  magnitudeBaseline = 1.0f;
  filteredImpact = 0.0f;
  calibrationSamples = 0;
  peakLatched = false;
}

bool initializeSensor() {
  if (!initialized) {
    // Wire is already configured by the firmware for the shared I2C bus.
    initialized = qmi.begin(Wire, QMI8658_L_SLAVE_ADDRESS);
  }
  return initialized;
}

bool configurePedometer(uint32_t initialTotal) {
  if (!initializeSensor()) return false;

  qmi.disablePedometer();
  qmi.disableGyroscope();
  qmi.disableAccelerometer();
  qmi.configAccelerometer(SensorQMI8658::ACC_RANGE_2G,
                          SensorQMI8658::ACC_ODR_62_5Hz);
  if (!qmi.enableAccelerometer()) return false;
  // A handheld toy moves less than a watch worn on a swinging wrist. Lower
  // the QMI8658 peak thresholds and accept a walking sequence after two
  // consecutive steps. The chip still enforces a ~0.2 s quiet time and updates
  // its autonomous counter every step, including while the ESP is asleep.
  qmi.configPedometer(40, 110, 50, 250, 12, 2, 0, 1);
  if (!qmi.clearPedometerCounter() ||
      !qmi.enablePedometer(SensorQMI8658::INTERRUPT_PIN_DISABLE)) {
    qmi.disableAccelerometer();
    return false;
  }
  hardwareSteps = 0;
  hardwareAnchorRaw = 0;
  hardwareAnchorTotal = initialTotal;
  resetSoftwareDetector(initialTotal);
  active = true;
  return true;
}
}

bool walkSensorStart() {
  return configurePedometer(0);
}

bool walkSensorRestore(uint32_t savedTotal, uint32_t savedRaw) {
  if (!initializeSensor()) return false;

  // The QMI8658 keeps its autonomous pedometer alive while the ESP is in
  // light sleep. If the ESP restarted during the 1.75C panel/touch wake path,
  // recover the delta before reconfiguring and clearing the hardware counter.
  const uint32_t currentRaw = qmi.getPedometerCounter();
  const uint32_t sleptSteps = currentRaw >= savedRaw ? currentRaw - savedRaw : 0;
  uint64_t restored = (uint64_t)savedTotal + sleptSteps;
  if (restored > 0xFFFFFFUL) restored = 0xFFFFFFUL;
  return configurePedometer((uint32_t)restored);
}

void walkSensorUpdate(uint32_t nowMs) {
  if (!active || (lastSampleAt && nowMs - lastSampleAt < 25UL)) return;
  lastSampleAt = nowMs;

  float x = 0.0f, y = 0.0f, z = 0.0f;
  if (!qmi.getAccelerometer(x, y, z)) return;
  const float magnitude = sqrtf(x * x + y * y + z * z);
  if (magnitude < 0.05f || magnitude > 4.0f) return;

  if (calibrationSamples < 12) {
    const float alpha = calibrationSamples == 0 ? 1.0f : 0.22f;
    gravityX += (x - gravityX) * alpha;
    gravityY += (y - gravityY) * alpha;
    gravityZ += (z - gravityZ) * alpha;
    magnitudeBaseline += (magnitude - magnitudeBaseline) * alpha;
    calibrationSamples++;
    return;
  }

  // Track gravity as a slowly moving 3-D vector. Scalar magnitude alone barely
  // changes when a user simply carries the device while walking; the dynamic
  // vector also sees the gentle rotation and swing of a normal hand-held gait.
  constexpr float kGravityAlpha = 0.018f;
  gravityX += (x - gravityX) * kGravityAlpha;
  gravityY += (y - gravityY) * kGravityAlpha;
  gravityZ += (z - gravityZ) * kGravityAlpha;
  const float dx = x - gravityX;
  const float dy = y - gravityY;
  const float dz = z - gravityZ;
  const float motion = sqrtf(dx * dx + dy * dy + dz * dz);
  filteredMotion = filteredMotion * 0.55f + motion * 0.45f;

  // Keep a scalar impact envelope for diagnostics and future calibration. The
  // actual step gate below uses motion hysteresis: a peak must fall back to a
  // quiet level before another step is accepted. That lets gentle hand-held
  // walking re-arm on every gait cycle while a device held at a new angle can
  // produce at most one transient count instead of repeating indefinitely.
  constexpr float kMagnitudeAlpha = 0.020f;
  magnitudeBaseline += (magnitude - magnitudeBaseline) * kMagnitudeAlpha;
  const float impact = fabsf(magnitude - magnitudeBaseline);
  filteredImpact = filteredImpact * 0.50f + impact * 0.50f;

  constexpr float kPeakThreshold = 0.052f;    // roughly 52 mg
  constexpr float kReleaseThreshold = 0.038f;
  // Limit repeated peaks from one vigorous hand shake without making the
  // acceleration threshold so high that ordinary walking is missed. At most
  // about three software steps can be accepted per second.
  constexpr uint32_t kMinimumStepMs = 340UL;
  if (!peakLatched && filteredMotion >= kPeakThreshold &&
      (!lastStepAt || nowMs - lastStepAt >= kMinimumStepMs)) {
    if (softwareSteps < 0xFFFFFFUL) softwareSteps++;
    lastStepAt = nowMs;
    peakLatched = true;
  } else if (peakLatched && filteredMotion <= kReleaseThreshold) {
    peakLatched = false;
  }
}

uint32_t walkSensorSteps() {
  if (!active) return 0;
  hardwareSteps = qmi.getPedometerCounter();
  // The active-walk screen-rest keeps the ESP awake, so the orientation-aware
  // software detector is authoritative. The QMI autonomous detector is much
  // too eager during deliberate shaking (often several counts per swing) and
  // is retained only as a restart/checkpoint reference.
  return softwareSteps;
}

uint32_t walkSensorRawSteps() {
  if (!active) return 0;
  hardwareSteps = qmi.getPedometerCounter();
  return hardwareSteps;
}

void walkSensorPrepareSleep() {
  if (!active) return;
  const uint32_t total = walkSensorSteps();
  // walkSensorSteps() refreshed hardwareSteps, so this anchor starts exactly
  // at sleep entry. From now on the autonomous QMI delta is added to the
  // already-earned unified total instead of competing with it via max().
  hardwareAnchorRaw = hardwareSteps;
  hardwareAnchorTotal = total;
  softwareSteps = total;
}

void walkSensorResumeFromSleep() {
  if (!active) return;
  const uint32_t total = walkSensorSteps();
  hardwareAnchorRaw = hardwareSteps;
  hardwareAnchorTotal = total;
  // Recalibrate gravity after being carried in a different orientation. Both
  // software and mapped hardware counters now continue from the same total.
  resetSoftwareDetector(total);
}

void walkSensorStop() {
  if (!active) return;
  qmi.disablePedometer();
  qmi.disableGyroscope();
  qmi.disableAccelerometer();
  active = false;
}

bool walkSensorActive() { return active; }
