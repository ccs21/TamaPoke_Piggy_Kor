#include "adpcm_clip.h"

#include <LittleFS.h>

namespace {

constexpr size_t HEADER_SIZE = 16;
constexpr int8_t INDEX_TABLE[16] = {
  -1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8
};
constexpr int16_t STEP_TABLE[89] = {
  7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31,
  34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118,
  130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371,
  408, 449, 494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166,
  1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024,
  3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845,
  8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500,
  20350, 22385, 24623, 27086, 29794, 32767
};

uint32_t readU32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
         ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

}  // namespace

bool AdpcmClip::load(const char *path) {
  unload();
  File file = LittleFS.open(path, FILE_READ);
  if (!file) return false;
  blobSize_ = file.size();
  if (blobSize_ < HEADER_SIZE) {
    file.close();
    return false;
  }
  blob_ = static_cast<uint8_t *>(ps_malloc(blobSize_));
  if (!blob_ || file.read(blob_, blobSize_) != blobSize_) {
    file.close();
    unload();
    return false;
  }
  file.close();
  if (memcmp(blob_, "TPA1", 4) != 0 || readU32(blob_ + 4) != 16000) {
    unload();
    return false;
  }
  sampleCount_ = readU32(blob_ + 8);
  memcpy(&initialPredictor_, blob_ + 12, sizeof(initialPredictor_));
  initialStepIndex_ = blob_[14];
  size_t payloadNeeded = sampleCount_ > 0 ? (sampleCount_ - 1 + 1) / 2 : 0;
  if (!sampleCount_ || initialStepIndex_ > 88 || HEADER_SIZE + payloadNeeded > blobSize_) {
    unload();
    return false;
  }
  payload_ = blob_ + HEADER_SIZE;
  rewind();
  return true;
}

void AdpcmClip::unload() {
  if (blob_) free(blob_);
  blob_ = nullptr;
  blobSize_ = 0;
  payload_ = nullptr;
  sampleCount_ = 0;
  samplePosition_ = 0;
}

void AdpcmClip::rewind() {
  samplePosition_ = 0;
  predictor_ = initialPredictor_;
  stepIndex_ = initialStepIndex_;
}

bool AdpcmClip::next(int16_t *sample) {
  if (!sample || !blob_ || samplePosition_ >= sampleCount_) return false;
  if (samplePosition_++ == 0) {
    *sample = (int16_t)predictor_;
    return true;
  }

  uint32_t codePosition = samplePosition_ - 2;
  uint8_t packed = payload_[codePosition >> 1];
  uint8_t code = (codePosition & 1) ? (packed >> 4) : (packed & 0x0F);
  int32_t step = STEP_TABLE[stepIndex_];
  int32_t difference = step >> 3;
  if (code & 1) difference += step >> 2;
  if (code & 2) difference += step >> 1;
  if (code & 4) difference += step;
  predictor_ += (code & 8) ? -difference : difference;
  predictor_ = constrain(predictor_, -32768, 32767);
  stepIndex_ = constrain(stepIndex_ + INDEX_TABLE[code], 0, 88);
  *sample = (int16_t)predictor_;
  return true;
}
