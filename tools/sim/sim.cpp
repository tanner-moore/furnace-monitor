// Furnace simulator for trying the web page without a CO16.
//
// Plays a thermostat schedule against a rough model of the White-Rodgers
// 50A51-495 sequence (inducer, ignition, two gas valve stages, blower delays,
// fault codes) plus air and flue temperatures, and feeds the result through the
// real furnace::Model from lib/furnace_core. Each simulated second becomes one
// JSON line with the same fields the firmware's /api/state sends.
//
// Usage: sim <start_epoch> [seed]
// Then write a number N on stdin to get the next N seconds as N lines, or "L"
// to play the showcase (one of each phase and fault) next.
// tools/sim/server.mjs drives it; see tools/sim/README.md.
//
// The timings and temperatures are plausible guesses, not measurements.

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <random>
#include <string>
#include <vector>

#include "flash_decoder.h"
#include "furnace_model.h"

using furnace::FlashDecoder;
using furnace::Inputs;
using furnace::Model;
using furnace::Phase;

namespace {

// --- thermostat schedule -------------------------------------------------

enum class Kind { Off, Heat, Fan, PressureFault, IgnitionFault };

struct Segment {
  Kind kind;
  int seconds;
  int w2AfterS = -1;  // Heat only: ecobee brings in W2 after this long, -1 = never
};

// What plays first once the page is open: one of everything.
std::vector<Segment> showcase() {
  return {
      {Kind::Off, 30},
      {Kind::Heat, 420, 180},  // low fire, then high fire after 3 minutes
      {Kind::Off, 150},        // post-purge and blower overrun happen here
      {Kind::Fan, 90},
      {Kind::Off, 30},
      {Kind::PressureFault, 150},  // inducer runs, pressure switch never closes: code 3
      {Kind::Off, 60},
      {Kind::IgnitionFault, 240},  // three failed trials, then lockout: code 2
      {Kind::Off, 60},
  };
}

class Schedule {
 public:
  explicit Schedule(unsigned seed) : rng_(seed) {}

  // Fills the queue with ordinary days: heat cycles, some long enough for
  // second stage, an occasional fan-only run.
  Segment next() {
    if (!queue_.empty()) {
      Segment s = queue_.front();
      queue_.erase(queue_.begin());
      return s;
    }
    if (pendingOff_) {
      pendingOff_ = false;
      return {Kind::Off, uni(900, 2400)};
    }
    pendingOff_ = true;
    if (uni(0, 9) == 0) return {Kind::Fan, uni(300, 900)};
    const int len = uni(360, 900);
    return {Kind::Heat, len, len > 600 ? 600 : -1};
  }

  void push(const std::vector<Segment>& segs) { queue_.insert(queue_.end(), segs.begin(), segs.end()); }

 private:
  int uni(int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(rng_); }
  std::mt19937 rng_;
  std::vector<Segment> queue_;
  bool pendingOff_ = false;
};

// --- control board -------------------------------------------------------

struct Board {
  // Timings loosely after the 50A51 manual.
  static constexpr int kPrepurgeS = 15, kIgnitorS = 17, kTrialS = 4, kInterpurgeS = 30, kTrials = 3;
  static constexpr int kBlowerOnDelayS = 45, kBlowerOffDelayS = 120, kPostPurgeS = 5, kLowInducerAfterS = 10;
  static constexpr int kHighValveDelayS = 5;

  enum class St { Idle, Prepurge, Ignitor, Trial, Interpurge, Firing, PostPurge, Lockout };
  St st = St::Idle;
  int stS = 0;  // seconds in current state
  int trial = 0;
  int firingS = 0, w2S = 0;
  int blowerOffInS = 0;  // > 0 while the off delay runs
  bool highBlower = false;
  int code = 0;

  bool mvl = false, mvh = false, inducer = false, inducerHigh = false, blowerHeat = false;

  void go(St s) { st = s, stS = 0; }

  void step(bool w1, bool w2, bool pressureFault, bool ignitionFault) {
    stS++;
    if (!w1 && st != St::Idle && st != St::PostPurge) {
      const bool wasFiring = st == St::Firing;
      go(wasFiring ? St::PostPurge : St::Idle);
      if (wasFiring) blowerOffInS = kBlowerOffDelayS;
      code = 0;  // removing the call resets a lockout
    }
    switch (st) {
      case St::Idle:
        if (w1) go(St::Prepurge), trial = 0;
        break;
      case St::Prepurge:
        if (pressureFault) code = stS > 30 ? 3 : 0;  // pressure switch never proves
        else if (stS >= kPrepurgeS) go(St::Ignitor);
        break;
      case St::Ignitor:
        if (stS >= kIgnitorS) go(St::Trial), trial++;
        break;
      case St::Trial:
        if (stS >= kTrialS) {
          if (!ignitionFault) go(St::Firing), firingS = kTrialS, w2S = 0;
          else if (trial >= kTrials) go(St::Lockout), code = 2;
          else go(St::Interpurge);
        }
        break;
      case St::Interpurge:
        if (stS >= kInterpurgeS) go(St::Ignitor);
        break;
      case St::Firing:
        firingS++;
        w2S = w2 ? w2S + 1 : 0;
        break;
      case St::PostPurge:
        if (stS >= kPostPurgeS) go(St::Idle);
        break;
      case St::Lockout:
        break;
    }

    const bool fire = st == St::Trial || st == St::Firing;
    mvl = fire;
    mvh = st == St::Firing && w2S > kHighValveDelayS;
    // The inducer takes a couple of seconds to draw current, so "heat call" shows briefly.
    inducer = (st == St::Prepurge && stS > 2) || st == St::Ignitor || st == St::Trial || st == St::Interpurge ||
              st == St::Firing || st == St::PostPurge;
    inducerHigh = inducer && !(st == St::Firing && firingS > kLowInducerAfterS && w2S == 0);

    if (st == St::Firing) {
      blowerOffInS = 0;
      if (firingS >= kBlowerOnDelayS) blowerHeat = true;
      highBlower = mvh;
    } else if (blowerHeat && blowerOffInS > 0) {
      if (--blowerOffInS == 0) blowerHeat = false;
    } else if (st != St::PostPurge) {
      blowerHeat = false;
    }
  }
};

// --- air and flue --------------------------------------------------------

double approach(double v, double target, double tauS) { return v + (target - v) * (1 - std::exp(-1.0 / tauS)); }

void putNum(std::string& out, const char* key, double v, int decimals = 1) {
  char buf[64];
  if (std::isnan(v)) snprintf(buf, sizeof buf, ",\"%s\":null", key);
  else snprintf(buf, sizeof buf, ",\"%s\":%.*f", key, decimals, v);
  out += buf;
}

void putBool(std::string& out, const char* key, bool v) {
  out += ",\"";
  out += key;
  out += v ? "\":true" : "\":false";
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: sim <start_epoch> [seed]\n");
    return 2;
  }
  const time_t start = (time_t)atoll(argv[1]);
  const unsigned seed = argc > 2 ? (unsigned)atoi(argv[2]) : 1;

  Schedule sched(seed);
  Model model;
  Board board;
  std::mt19937 noise(seed + 1);
  std::normal_distribution<double> jitter(0, 0.08);

  Segment seg = sched.next();
  int segLeft = seg.seconds, segS = 0;
  int lastYday = -1;
  long t = 0;
  double ret = 19.5, sup = 21.0, flue = 28.0;

  char line[64];
  while (fgets(line, sizeof line, stdin)) {
    // "L" switches from history to live: wind down whatever is running and
    // play the showcase next. A number N asks for the next N seconds.
    if (line[0] == 'L') {
      seg.kind = Kind::Off;
      segLeft = std::min(segLeft, 5);
      sched.push(showcase());
      continue;
    }
    const long n = atol(line);
    for (long i = 0; i < n; i++, t++) {
      if (segLeft-- <= 0) {
        seg = sched.next();
        segLeft = seg.seconds - 1;
        segS = 0;
      }
      segS++;

      const bool heat = seg.kind == Kind::Heat || seg.kind == Kind::PressureFault || seg.kind == Kind::IgnitionFault;
      const bool w1 = heat;
      const bool w2 = seg.kind == Kind::Heat && seg.w2AfterS >= 0 && segS >= seg.w2AfterS;
      const bool g = seg.kind == Kind::Fan;
      board.step(w1, w2, seg.kind == Kind::PressureFault, seg.kind == Kind::IgnitionFault);

      // Currents.
      Inputs in;
      in.w1 = w1;
      in.w2 = w2;
      in.g = g;
      in.mvl = board.mvl;
      in.mvh = board.mvh;
      in.inducerAmps = board.inducer ? (board.inducerHigh ? 1.62 : 0.88) + jitter(noise) * 0.2 : 0.02;
      double blowerA = 0.05;
      if (board.blowerHeat) blowerA = board.highBlower ? 5.6 : 3.9;
      else if (g) blowerA = 2.7;
      in.blowerAmps = blowerA + (blowerA > 1 ? jitter(noise) : 0);
      in.boardCode = board.code;

      // Temperatures in C. Return drifts with the house; supply follows the
      // heat exchanger; the strap-on flue probe lags the burner.
      const bool blowing = in.blowerAmps > 1;
      const double rise = board.mvh ? 33.0 : board.mvl ? 22.0 : 0;
      ret = approach(ret, blowing ? 20.2 : 19.4, 900);
      if (board.mvl && !blowing) sup = approach(sup, ret + 38, 70);  // no airflow yet: plenum heats up
      else if (board.mvl) sup = approach(sup, ret + rise, 50);
      else if (blowing) sup = approach(sup, ret + 0.8, 35);
      else sup = approach(sup, ret + 1.5, 400);
      const double flueTarget = board.mvh ? 150 : board.mvl ? 112 : board.inducer ? 26 : 30;
      flue = approach(flue, flueTarget, board.mvl ? 50 : 240);
      in.supplyC = sup + jitter(noise);
      in.returnC = ret + jitter(noise) * 0.5;
      in.flueC = flue + jitter(noise) * 2;

      // Day counters reset at local midnight, as on the board.
      const time_t epoch = start + t;
      struct tm lt;
      localtime_r(&epoch, &lt);
      if (lastYday >= 0 && lt.tm_yday != lastYday) model.newDay();
      lastYday = lt.tm_yday;

      const Phase before = model.state().phase;
      const uint32_t ev = model.update(in, (uint32_t)(t * 1000));
      const auto& st = model.state();

      std::string events;
      auto addEvent = [&](const char* text) {
        events += events.empty() ? "\"" : ",\"";
        events += text;
        events += "\"";
      };
      char buf[320];
      if (ev & (1u << (int)furnace::Event::PhaseChanged)) {
        snprintf(buf, sizeof buf, "%s -> %s", furnace::phaseName(before), furnace::phaseName(st.phase));
        addEvent(buf);
      }
      if (ev & (1u << (int)furnace::Event::CycleEnded)) {
        snprintf(buf, sizeof buf, "burner cycle ended after %lu s", (unsigned long)st.lastCycleSeconds);
        addEvent(buf);
      }
      if (model.newAlerts()) {
        char names[160];
        furnace::alertNames(model.newAlerts(), names, sizeof names);
        snprintf(buf, sizeof buf, "alert: %s", names);
        addEvent(buf);
      }

      char alerts[200];
      furnace::alertNames(st.alerts, alerts, sizeof alerts);
      const unsigned raw = (in.w1 ? 1 : 0) | (in.w2 ? 2 : 0) | (in.g ? 8 : 0) | (in.mvl ? 16 : 0) | (in.mvh ? 32 : 0);
      const unsigned flags = (in.w1 ? 1 : 0) | (in.w2 ? 2 : 0) | (in.g ? 4 : 0) | (in.mvl ? 8 : 0) |
                             (in.mvh ? 16 : 0) | (st.flame ? 32 : 0) | (st.blowerOn ? 64 : 0);

      std::string out = "{\"phase\":\"";
      out += furnace::phaseName(st.phase);
      out += "\"";
      putBool(out, "heat_call", in.w1);
      putBool(out, "heat_call_2", in.w2);
      putBool(out, "fan_call", in.g);
      putBool(out, "burner", in.mvl || in.mvh);
      putBool(out, "high_fire", in.mvh);
      putBool(out, "flame", st.flame);
      putBool(out, "inducer", st.inducerOn);
      putBool(out, "inducer_high", st.inducerHigh);
      putBool(out, "blower", st.blowerOn);
      putNum(out, "t_supply", in.supplyC);
      putNum(out, "t_return", in.returnC);
      putNum(out, "delta_t", st.deltaTC);
      putNum(out, "t_flue", in.flueC);
      putNum(out, "t_spare", NAN);
      putNum(out, "i_inducer", in.inducerAmps, 2);
      putNum(out, "i_blower", in.blowerAmps, 2);
      snprintf(buf, sizeof buf,
               ",\"cycles_today\":%u,\"burner_min_today\":%u,\"high_fire_min_today\":%u,\"last_cycle_s\":%u",
               st.cyclesToday, st.burnerSecondsToday / 60, st.highFireSecondsToday / 60, st.lastCycleSeconds);
      out += buf;
      putBool(out, "problem", st.alerts != 0);
      snprintf(buf, sizeof buf, ",\"alerts\":\"%s\",\"board_code\":%d,\"board_status\":\"%s\"", alerts,
               in.boardCode, FlashDecoder::describe(in.boardCode));
      out += buf;
      snprintf(buf, sizeof buf, ",\"inputs\":%u,\"uptime_s\":%ld,\"time\":%lld,\"flags\":%u,\"events\":[", raw, t,
               (long long)epoch, flags);
      out += buf;
      out += events;
      out += "]}\n";
      fputs(out.c_str(), stdout);
    }
    fflush(stdout);
  }
  return 0;
}
