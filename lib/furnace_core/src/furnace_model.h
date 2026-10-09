// Furnace state model: turns raw input readings into a phase, flame status,
// cycle statistics and alerts. Pure C++ with no Arduino dependencies so it can
// be unit tested on the host (see test/test_furnace_model).
//
// Tuned for a two-stage furnace on a White-Rodgers 50A51 control:
// W1/W2 heat calls, MVL/MVH gas valve outputs, two-speed inducer.
#pragma once

#include <cmath>
#include <cstdint>

namespace furnace {

enum class Phase : uint8_t {
  Idle,          // nothing calling, nothing running
  FanOnly,       // G call, blower without heat
  HeatCall,      // W1 active, inducer not (yet) seen running
  Ignition,      // W1 + inducer running, gas valve still closed (purge/ignitor warm-up)
  LowFire,       // MVL open, MVH closed
  HighFire,      // MVH open
  PostPurge,     // call ended, inducer still running
  BlowerOverrun, // call ended, blower running off its delay
  Lockout,       // control board reports a lockout code
};

const char* phaseName(Phase p);

// Alert bits; several can be active at once.
enum Alert : uint32_t {
  ALERT_NONE = 0,
  ALERT_IGNITION_FAILURE = 1u << 0,   // heat call with no burner after ignitionTimeoutS
  ALERT_BURNER_WITHOUT_CALL = 1u << 1, // gas valve open with no W1
  ALERT_FLAME_LOSS = 1u << 2,         // valve dropped mid-call after flame was proven
  ALERT_SHORT_CYCLE = 1u << 3,        // last burner cycle shorter than shortCycleS
  ALERT_LOW_DELTA_T = 1u << 4,        // supply-return rise below minDeltaTC while firing
  ALERT_SUPPLY_HIGH = 1u << 5,        // supply air above maxSupplyC
  ALERT_BLOWER_NOT_RUNNING = 1u << 6, // firing past blower delay with no blower current
  ALERT_FLUE_NO_RISE = 1u << 7,       // firing but flue temperature not rising
  ALERT_SENSOR_FAULT = 1u << 8,       // a configured sensor reads invalid
  ALERT_BOARD_FAULT = 1u << 9,        // control board LED shows a fault code
};

// Writes a comma-separated list of alert names into buf. Returns buf.
const char* alertNames(uint32_t alerts, char* buf, size_t len);

// One reading of every input. Unconfigured analog values are NaN.
struct Inputs {
  bool w1 = false;
  bool w2 = false;
  bool g = false;
  bool mvl = false;
  bool mvh = false;
  float inducerAmps = NAN;
  float blowerAmps = NAN;
  float supplyC = NAN;
  float returnC = NAN;
  float flueC = NAN;
  bool sensorFault = false;  // set by the driver when an RTD/ADC reports a fault
  int boardCode = 0;         // 0 = normal/unknown, else flash count (or 99 = steady on)
};

struct Settings {
  float inducerOnAmps = 0.3f;      // above this the inducer counts as running
  float inducerHighAmps = 1.2f;    // above this the inducer counts as high speed
  float blowerOnAmps = 1.0f;       // above this the blower counts as running
  uint32_t flameConfirmS = 5;      // valve held this long = board has proven flame
  uint32_t ignitionTimeoutS = 90;  // W1 without valve for this long = ignition failure
  uint32_t shortCycleS = 180;      // burner cycles shorter than this are short cycles
  uint32_t blowerDelayS = 90;      // allowed time from flame to blower current (board: 45 s)
  uint32_t settleS = 300;          // firing time before delta-T and flue checks apply
  float minDeltaTC = 15.0f;        // supply minus return while firing
  float maxSupplyC = 85.0f;        // supply air limit
  float minFlueRiseC = 10.0f;      // flue rise expected after settleS of firing
};

struct State {
  Phase phase = Phase::Idle;
  bool flame = false;
  bool inducerOn = false;
  bool inducerHigh = false;
  bool blowerOn = false;
  float deltaTC = NAN;
  uint32_t alerts = ALERT_NONE;

  // Statistics for the current day (reset by Model::newDay()).
  uint32_t cyclesToday = 0;
  uint32_t burnerSecondsToday = 0;
  uint32_t highFireSecondsToday = 0;
  uint32_t lastCycleSeconds = 0;
};

// Events the caller may want to log or publish.
enum class Event : uint8_t { None, PhaseChanged, CycleStarted, CycleEnded, AlertRaised, AlertCleared };

class Model {
 public:
  explicit Model(const Settings& s = Settings()) : s_(s) {}

  void setSettings(const Settings& s) { s_ = s; }
  const Settings& settings() const { return s_; }

  // Feed one reading. nowMs must be monotonic (millis()). Returns a bitmask of
  // (1 << Event) values that happened during this update.
  uint32_t update(const Inputs& in, uint32_t nowMs);

  // Zero the per-day counters (call at local midnight).
  void newDay();

  // Restore counters after a reboot.
  void restoreDay(uint32_t cycles, uint32_t burnerS, uint32_t highFireS);

  const State& state() const { return st_; }
  uint32_t newAlerts() const { return raised_; }

 private:
  Settings s_;
  State st_;
  uint32_t raised_ = 0;

  bool started_ = false;
  uint32_t lastMs_ = 0;
  uint32_t msCarryBurner_ = 0;
  uint32_t msCarryHigh_ = 0;

  bool valveWas_ = false;
  uint32_t valveSinceMs_ = 0;
  uint32_t w1SinceMs_ = 0;
  bool w1Was_ = false;
  bool valveSeenThisCall_ = false;
  bool flameSeenThisCall_ = false;
  bool noCallValve_ = false;
  uint32_t noCallValveSinceMs_ = 0;
  float flueAtValveOpen_ = NAN;
  bool flameLossLatched_ = false;
  bool shortCycleLatched_ = false;
};

}  // namespace furnace
