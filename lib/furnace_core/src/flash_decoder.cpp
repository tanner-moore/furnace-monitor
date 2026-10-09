#include "flash_decoder.h"

namespace furnace {

const char* FlashDecoder::describe(int code) {
  switch (code) {
    case kCodeNormal: return "normal";
    case 2: return "lockout, no flame";
    case 3: return "pressure switch problem";
    case 4: return "limit switch open";
    case 5: return "flame sensed with gas valve off";
    case 6: return "115 V polarity reversed";
    case 7: return "gas valve circuit error";
    case 8: return "low flame signal";
    case kCodeSteadyOn: return "internal fault";
  }
  return "unknown code";
}

void FlashDecoder::endGroup() {
  if (count_ == 0) return;
  // A single flash per group is the normal heartbeat.
  if (count_ == 1) {
    code_ = kCodeNormal;
  } else if (count_ == lastGroup_) {
    code_ = count_;
  }
  lastGroup_ = count_;
  count_ = 0;
}

void FlashDecoder::sample(bool lit, uint32_t nowMs) {
  if (!started_) {
    started_ = true;
    lit_ = lit;
    edgeMs_ = nowMs;
    lastFlashMs_ = nowMs;
    return;
  }

  if (lit != lit_) {
    lit_ = lit;
    edgeMs_ = nowMs;
    if (lit) {
      count_++;
      lastFlashMs_ = nowMs;
      // A continuous blink (slow or fast heartbeat) never pauses long enough to
      // end a group; past the highest code it can only be the heartbeat.
      if (count_ > 9) {
        code_ = kCodeNormal;
        count_ = 0;
        lastGroup_ = -1;
      }
    }
    return;
  }

  const uint32_t steady = nowMs - edgeMs_;
  if (lit && steady >= s_.steadyOnMs) {
    code_ = kCodeSteadyOn;
    count_ = 0;
    lastGroup_ = -1;
  } else if (!lit && steady >= s_.groupGapMs) {
    endGroup();
    if (nowMs - lastFlashMs_ >= s_.staleMs) {
      code_ = kCodeNormal;
      lastGroup_ = -1;
    }
  }
}

}  // namespace furnace
