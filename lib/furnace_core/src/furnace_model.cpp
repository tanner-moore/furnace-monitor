#include "furnace_model.h"

#include <cstdio>
#include <cstring>

namespace furnace {

const char* phaseName(Phase p) {
  switch (p) {
    case Phase::Idle: return "idle";
    case Phase::FanOnly: return "fan_only";
    case Phase::HeatCall: return "heat_call";
    case Phase::Ignition: return "ignition";
    case Phase::LowFire: return "low_fire";
    case Phase::HighFire: return "high_fire";
    case Phase::PostPurge: return "post_purge";
    case Phase::BlowerOverrun: return "blower_overrun";
    case Phase::Lockout: return "lockout";
  }
  return "unknown";
}

const char* alertNames(uint32_t alerts, char* buf, size_t len) {
  static const struct { uint32_t bit; const char* name; } kNames[] = {
      {ALERT_IGNITION_FAILURE, "ignition_failure"},
      {ALERT_BURNER_WITHOUT_CALL, "burner_without_call"},
      {ALERT_FLAME_LOSS, "flame_loss"},
      {ALERT_SHORT_CYCLE, "short_cycle"},
      {ALERT_LOW_DELTA_T, "low_delta_t"},
      {ALERT_SUPPLY_HIGH, "supply_high"},
      {ALERT_BLOWER_NOT_RUNNING, "blower_not_running"},
      {ALERT_FLUE_NO_RISE, "flue_no_rise"},
      {ALERT_SENSOR_FAULT, "sensor_fault"},
      {ALERT_BOARD_FAULT, "board_fault"},
      {ALERT_CO_ALARM, "co_alarm"},
  };
  if (len == 0) return buf;
  buf[0] = '\0';
  size_t used = 0;
  for (const auto& n : kNames) {
    if (!(alerts & n.bit)) continue;
    int w = snprintf(buf + used, len - used, "%s%s", used ? "," : "", n.name);
    if (w < 0 || (size_t)w >= len - used) break;
    used += (size_t)w;
  }
  return buf;
}

static inline uint32_t elapsedS(uint32_t nowMs, uint32_t sinceMs) { return (nowMs - sinceMs) / 1000; }

void Model::newDay() {
  st_.cyclesToday = 0;
  st_.burnerSecondsToday = 0;
  st_.highFireSecondsToday = 0;
  msCarryBurner_ = 0;
  msCarryHigh_ = 0;
}

void Model::restoreDay(uint32_t cycles, uint32_t burnerS, uint32_t highFireS) {
  st_.cyclesToday = cycles;
  st_.burnerSecondsToday = burnerS;
  st_.highFireSecondsToday = highFireS;
}

// Adds dtMs to a millisecond carry and returns the whole seconds it completes.
static inline uint32_t addTime(uint32_t& carryMs, uint32_t dtMs) {
  carryMs += dtMs;
  const uint32_t whole = carryMs / 1000;
  carryMs %= 1000;
  return whole;
}

static inline float average(double sum, uint32_t ms) { return ms ? (float)(sum / ms) : NAN; }

uint32_t Model::update(const Inputs& in, uint32_t nowMs) {
  uint32_t events = 0;
  const uint32_t dtMs = started_ ? nowMs - lastMs_ : 0;
  started_ = true;
  lastMs_ = nowMs;

  // Motors: use current when the CT is fitted, otherwise infer from the calls.
  const bool valve = in.mvl || in.mvh;
  st_.inducerOn = std::isnan(in.inducerAmps) ? (in.w1 || valve) : in.inducerAmps >= s_.inducerOnAmps;
  st_.inducerHigh = !std::isnan(in.inducerAmps) && in.inducerAmps >= s_.inducerHighAmps;
  st_.blowerOn = std::isnan(in.blowerAmps) ? in.g : in.blowerAmps >= s_.blowerOnAmps;

  // Track the heat call.
  if (in.w1 && !w1Was_) {
    w1SinceMs_ = nowMs;
    valveSeenThisCall_ = false;
    flameSeenThisCall_ = false;
    flameLossLatched_ = false;
    st_.failedTrialsThisCall = 0;
  }
  w1Was_ = in.w1;

  st_.deltaTC = (std::isnan(in.supplyC) || std::isnan(in.returnC)) ? NAN : in.supplyC - in.returnC;

  // Track the gas valve. Each opening is an ignition trial; it becomes a burner
  // cycle once the valve is held long enough to prove flame.
  if (valve && !valveWas_) {
    valveSinceMs_ = nowMs;
    valveSeenThisCall_ = true;
    flueAtValveOpen_ = in.flueC;
    cycleFlame_ = false;
    cycleIgnitionS_ = in.w1 ? elapsedS(nowMs, w1SinceMs_) : 0;
    cycleHighMs_ = 0;
    highWas_ = in.mvh;
    stageSinceMs_ = nowMs;
    flueMax_ = NAN;
    riseLowSum_ = riseHighSum_ = inducerSum_ = blowerSum_ = 0;
    riseLowMs_ = riseHighMs_ = inducerMs_ = blowerMs_ = 0;
    events |= 1u << (int)Event::CycleStarted;
  }

  if (valve) {
    if (in.mvh != highWas_) {
      highWas_ = in.mvh;
      stageSinceMs_ = nowMs;
    }
    if (in.mvh) cycleHighMs_ += dtMs;
    const bool settled = elapsedS(nowMs, stageSinceMs_) >= s_.riseSettleS;
    if (settled && !std::isnan(st_.deltaTC)) {
      if (in.mvh) {
        riseHighSum_ += (double)st_.deltaTC * dtMs;
        riseHighMs_ += dtMs;
      } else {
        riseLowSum_ += (double)st_.deltaTC * dtMs;
        riseLowMs_ += dtMs;
      }
    }
    if (!std::isnan(in.flueC) && (std::isnan(flueMax_) || in.flueC > flueMax_)) flueMax_ = in.flueC;
    if (!std::isnan(in.inducerAmps)) {
      inducerSum_ += (double)in.inducerAmps * dtMs;
      inducerMs_ += dtMs;
    }
    if (!std::isnan(in.blowerAmps) && st_.blowerOn) {
      blowerSum_ += (double)in.blowerAmps * dtMs;
      blowerMs_ += dtMs;
    }
  }

  if (!valve && valveWas_) {
    if (cycleFlame_) {
      CycleStats& c = st_.lastCycle;
      c.seconds = elapsedS(nowMs, valveSinceMs_);
      c.highFireSeconds = cycleHighMs_ / 1000;
      c.ignitionSeconds = cycleIgnitionS_;
      c.riseLowC = average(riseLowSum_, riseLowMs_);
      c.riseHighC = average(riseHighSum_, riseHighMs_);
      c.flueMaxC = flueMax_;
      c.inducerAmps = average(inducerSum_, inducerMs_);
      c.blowerAmps = average(blowerSum_, blowerMs_);
      st_.lastCycleSeconds = c.seconds;
      st_.cyclesToday++;
      st_.totals.cycles++;
      shortCycleLatched_ = c.seconds < s_.shortCycleS;
      if (in.w1) flameLossLatched_ = true;
      events |= 1u << (int)Event::CycleEnded;
    } else {
      // The board opened the valve for a trial and closed it without proving
      // flame (the 50A51 retries a few times, then locks out).
      st_.failedTrialsThisCall++;
      st_.totals.failedTrials++;
      if (st_.failedTrialsThisCall >= s_.ignitionTrials) ignitionFailLatched_ = true;
      events |= 1u << (int)Event::TrialFailed;
    }
  }
  valveWas_ = valve;

  // Runtime accounting with millisecond carry so short updates are not lost.
  if (valve) {
    const uint32_t s = addTime(msCarryBurner_, dtMs);
    st_.burnerSecondsToday += s;
    st_.totals.burnerSeconds += s;
  }
  if (in.mvh) {
    const uint32_t s = addTime(msCarryHigh_, dtMs);
    st_.highFireSecondsToday += s;
    st_.totals.highFireSeconds += s;
  }
  if (st_.blowerOn) {
    const uint32_t s = addTime(msCarryBlower_, dtMs);
    st_.totals.blowerSeconds += s;
    st_.totals.filterBlowerSeconds += s;
  }

  // The 50A51 only holds the valve open while it proves flame, so a valve held
  // past flameConfirmS means flame.
  st_.flame = valve && elapsedS(nowMs, valveSinceMs_) >= s_.flameConfirmS;
  if (st_.flame) {
    flameSeenThisCall_ = true;
    cycleFlame_ = true;
    st_.failedTrialsThisCall = 0;
    ignitionFailLatched_ = false;
  }

  // Phase.
  Phase phase;
  const bool lockout = in.boardCode == 2 || in.boardCode == 99;
  if (lockout) {
    phase = Phase::Lockout;
  } else if (in.mvh) {
    phase = Phase::HighFire;
  } else if (in.mvl) {
    phase = Phase::LowFire;
  } else if (in.w1) {
    phase = st_.inducerOn ? Phase::Ignition : Phase::HeatCall;
  } else if (st_.inducerOn) {
    phase = Phase::PostPurge;
  } else if (in.g) {
    phase = Phase::FanOnly;
  } else if (st_.blowerOn) {
    phase = Phase::BlowerOverrun;
  } else {
    phase = Phase::Idle;
  }
  if (phase != st_.phase) events |= 1u << (int)Event::PhaseChanged;
  st_.phase = phase;

  // Alerts.
  uint32_t a = 0;
  if (in.w1 && !valveSeenThisCall_ && elapsedS(nowMs, w1SinceMs_) >= s_.ignitionTimeoutS) ignitionFailLatched_ = true;
  if (ignitionFailLatched_) a |= ALERT_IGNITION_FAILURE;

  if (valve && !in.w1) {
    if (!noCallValve_) noCallValveSinceMs_ = nowMs;
    noCallValve_ = true;
    if (elapsedS(nowMs, noCallValveSinceMs_) >= 5) a |= ALERT_BURNER_WITHOUT_CALL;
  } else {
    noCallValve_ = false;
  }

  if (!in.w1) flameLossLatched_ = false;
  if (flameLossLatched_) a |= ALERT_FLAME_LOSS;
  if (shortCycleLatched_) a |= ALERT_SHORT_CYCLE;

  const uint32_t firingS = valve ? elapsedS(nowMs, valveSinceMs_) : 0;
  if (valve && firingS >= s_.settleS && !std::isnan(st_.deltaTC) && st_.deltaTC < s_.minDeltaTC) a |= ALERT_LOW_DELTA_T;
  if (!std::isnan(in.supplyC) && in.supplyC > s_.maxSupplyC) a |= ALERT_SUPPLY_HIGH;
  if (valve && firingS >= s_.blowerDelayS && !std::isnan(in.blowerAmps) && !st_.blowerOn) a |= ALERT_BLOWER_NOT_RUNNING;
  if (valve && firingS >= s_.settleS && !std::isnan(in.flueC) && !std::isnan(flueAtValveOpen_) &&
      in.flueC - flueAtValveOpen_ < s_.minFlueRiseC)
    a |= ALERT_FLUE_NO_RISE;
  if (in.sensorFault) a |= ALERT_SENSOR_FAULT;
  if (in.boardCode >= 2) a |= ALERT_BOARD_FAULT;
  if (in.coAlarm) a |= ALERT_CO_ALARM;

  raised_ = a & ~st_.alerts;
  if (raised_) events |= 1u << (int)Event::AlertRaised;
  if (st_.alerts & ~a) events |= 1u << (int)Event::AlertCleared;
  st_.alerts = a;

  return events;
}

}  // namespace furnace
