#include <unity.h>

#include "furnace_model.h"

using namespace furnace;

void setUp() {}
void tearDown() {}

// Steps the model once per second from t0 to t1 (exclusive) with fixed inputs.
static void run(Model& m, const Inputs& in, uint32_t& t, uint32_t seconds) {
  for (uint32_t i = 0; i < seconds; i++, t += 1000) m.update(in, t);
}

static void test_idle_by_default() {
  Model m;
  m.update(Inputs(), 0);
  TEST_ASSERT_EQUAL(Phase::Idle, m.state().phase);
  TEST_ASSERT_FALSE(m.state().flame);
  TEST_ASSERT_EQUAL_UINT32(0, m.state().alerts);
}

static void test_normal_two_stage_cycle() {
  Model m;
  uint32_t t = 0;
  Inputs in;
  in.inducerAmps = 0.0f;
  in.blowerAmps = 0.0f;
  in.supplyC = 20.0f;
  in.returnC = 20.0f;
  in.flueC = 25.0f;

  in.w1 = true;
  run(m, in, t, 2);
  TEST_ASSERT_EQUAL(Phase::HeatCall, m.state().phase);

  in.inducerAmps = 1.5f;  // starts on high speed
  run(m, in, t, 20);
  TEST_ASSERT_EQUAL(Phase::Ignition, m.state().phase);
  TEST_ASSERT_TRUE(m.state().inducerHigh);

  in.inducerAmps = 0.8f;  // drops to low, valve opens
  in.mvl = true;
  run(m, in, t, 3);
  TEST_ASSERT_EQUAL(Phase::LowFire, m.state().phase);
  TEST_ASSERT_FALSE(m.state().flame);  // not yet held for flameConfirmS
  run(m, in, t, 5);
  TEST_ASSERT_TRUE(m.state().flame);

  in.blowerAmps = 4.0f;
  in.supplyC = 50.0f;
  in.flueC = 150.0f;
  run(m, in, t, 300);
  TEST_ASSERT_TRUE(m.state().blowerOn);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 30.0f, m.state().deltaTC);

  in.w2 = true;
  in.mvh = true;
  in.inducerAmps = 1.5f;
  run(m, in, t, 60);
  TEST_ASSERT_EQUAL(Phase::HighFire, m.state().phase);
  TEST_ASSERT_EQUAL_UINT32(60, m.state().highFireSecondsToday);

  in.w1 = in.w2 = in.mvl = in.mvh = false;
  m.update(in, t);
  TEST_ASSERT_EQUAL(Phase::PostPurge, m.state().phase);
  TEST_ASSERT_EQUAL_UINT32(1, m.state().cyclesToday);
  TEST_ASSERT_EQUAL_UINT32(368, m.state().lastCycleSeconds);

  in.inducerAmps = 0.0f;
  t += 5000;
  m.update(in, t);
  TEST_ASSERT_EQUAL(Phase::BlowerOverrun, m.state().phase);

  in.blowerAmps = 0.0f;
  t += 100000;
  m.update(in, t);
  TEST_ASSERT_EQUAL(Phase::Idle, m.state().phase);
  TEST_ASSERT_EQUAL_UINT32(0, m.state().alerts);
}

static void test_ignition_failure() {
  Model m;
  uint32_t t = 0;
  Inputs in;
  in.w1 = true;
  run(m, in, t, 89);
  TEST_ASSERT_EQUAL_UINT32(0, m.state().alerts & ALERT_IGNITION_FAILURE);
  run(m, in, t, 2);
  TEST_ASSERT_TRUE(m.state().alerts & ALERT_IGNITION_FAILURE);
}

// The 50A51 opens the valve for a short trial each time it tries to light, so
// a failed ignition shows as brief valve openings, not as a closed valve.
static void test_ignition_failure_with_valve_trials() {
  Model m;
  uint32_t t = 0;
  Inputs in;
  in.w1 = true;
  run(m, in, t, 30);  // purge and ignitor warm-up
  for (int trial = 1; trial <= 3; trial++) {
    in.mvl = true;
    run(m, in, t, 4);  // trial for ignition, no flame proven
    in.mvl = false;
    run(m, in, t, 30);  // inter-purge
    TEST_ASSERT_EQUAL_UINT32(trial, m.state().failedTrialsThisCall);
    if (trial == 1) TEST_ASSERT_EQUAL_UINT32(0, m.state().alerts & ALERT_IGNITION_FAILURE);
    else TEST_ASSERT_TRUE(m.state().alerts & ALERT_IGNITION_FAILURE);
  }
  // Failed trials are not burner cycles and not short cycles.
  TEST_ASSERT_EQUAL_UINT32(0, m.state().cyclesToday);
  TEST_ASSERT_EQUAL_UINT32(0, m.state().alerts & ALERT_SHORT_CYCLE);
  TEST_ASSERT_EQUAL_UINT32(3, m.state().totals.failedTrials);

  // Still latched after the call ends (the board is in lockout)...
  in.w1 = false;
  run(m, in, t, 10);
  TEST_ASSERT_TRUE(m.state().alerts & ALERT_IGNITION_FAILURE);

  // ...until the burner lights properly.
  in.w1 = in.mvl = true;
  run(m, in, t, 10);
  TEST_ASSERT_TRUE(m.state().flame);
  TEST_ASSERT_EQUAL_UINT32(0, m.state().alerts & ALERT_IGNITION_FAILURE);
  TEST_ASSERT_EQUAL_UINT32(0, m.state().failedTrialsThisCall);
}

static void test_one_failed_trial_then_light() {
  Model m;
  uint32_t t = 0;
  Inputs in;
  in.w1 = true;
  run(m, in, t, 30);
  in.mvl = true;
  run(m, in, t, 4);
  in.mvl = false;
  run(m, in, t, 30);
  in.mvl = true;
  run(m, in, t, 600);
  TEST_ASSERT_EQUAL_UINT32(0, m.state().alerts);
  in.w1 = in.mvl = false;
  m.update(in, t);
  TEST_ASSERT_EQUAL_UINT32(1, m.state().cyclesToday);
  TEST_ASSERT_EQUAL_UINT32(1, m.state().totals.failedTrials);
  TEST_ASSERT_EQUAL_UINT32(64, m.state().lastCycle.ignitionSeconds);
}

static void test_cycle_stats() {
  Model m;
  uint32_t t = 0;
  Inputs in;
  in.inducerAmps = 0.0f;
  in.blowerAmps = 0.0f;
  in.supplyC = in.returnC = 20.0f;
  in.flueC = 25.0f;
  in.w1 = true;
  in.inducerAmps = 0.8f;
  run(m, in, t, 25);
  in.mvl = true;
  run(m, in, t, 45);  // blower comes on after 45 s
  in.blowerAmps = 4.0f;
  in.supplyC = 50.0f;
  in.flueC = 140.0f;
  run(m, in, t, 300);  // low fire: settles after riseSettleS, rise 30
  in.w2 = in.mvh = true;
  in.inducerAmps = 1.6f;
  in.blowerAmps = 6.0f;
  in.supplyC = 60.0f;
  in.flueC = 180.0f;
  run(m, in, t, 400);  // high fire: rise 40
  in.w1 = in.w2 = in.mvl = in.mvh = false;
  m.update(in, t);

  const CycleStats& c = m.state().lastCycle;
  TEST_ASSERT_EQUAL_UINT32(745, c.seconds);
  TEST_ASSERT_EQUAL_UINT32(400, c.highFireSeconds);
  TEST_ASSERT_EQUAL_UINT32(25, c.ignitionSeconds);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 30.0f, c.riseLowC);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 40.0f, c.riseHighC);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 180.0f, c.flueMaxC);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 5.143f, c.blowerAmps);  // 300 s at 4 A, 400 s at 6 A
  TEST_ASSERT_TRUE(c.inducerAmps > 0.8f && c.inducerAmps < 1.6f);
  TEST_ASSERT_EQUAL_UINT32(1, m.state().totals.cycles);
  TEST_ASSERT_EQUAL_UINT32(745, m.state().totals.burnerSeconds);
  TEST_ASSERT_EQUAL_UINT32(400, m.state().totals.highFireSeconds);
}

static void test_rise_needs_settled_stage() {
  Model m;
  uint32_t t = 0;
  Inputs in;
  in.w1 = in.mvl = true;
  in.supplyC = 45.0f;
  in.returnC = 20.0f;
  run(m, in, t, 100);  // shorter than riseSettleS
  in.w1 = in.mvl = false;
  m.update(in, t);
  TEST_ASSERT_TRUE(std::isnan(m.state().lastCycle.riseLowC));
  TEST_ASSERT_TRUE(std::isnan(m.state().lastCycle.riseHighC));
}

static void test_filter_blower_time() {
  Model m;
  uint32_t t = 0;
  Inputs in;
  in.g = true;  // no blower CT: fan call means blower on
  run(m, in, t, 3601);
  TEST_ASSERT_EQUAL_UINT32(3600, m.state().totals.filterBlowerSeconds);
  TEST_ASSERT_EQUAL_UINT32(3600, m.state().totals.blowerSeconds);
  m.resetFilter();
  run(m, in, t, 10);
  TEST_ASSERT_EQUAL_UINT32(10, m.state().totals.filterBlowerSeconds);
  TEST_ASSERT_EQUAL_UINT32(3610, m.state().totals.blowerSeconds);

  Totals saved;
  saved.cycles = 7;
  saved.filterBlowerSeconds = 99;
  m.restoreTotals(saved);
  TEST_ASSERT_EQUAL_UINT32(7, m.state().totals.cycles);
  TEST_ASSERT_EQUAL_UINT32(99, m.state().totals.filterBlowerSeconds);
}

static void test_co_alarm() {
  Model m;
  Inputs in;
  in.coAlarm = true;
  m.update(in, 0);
  TEST_ASSERT_TRUE(m.state().alerts & ALERT_CO_ALARM);
  char buf[32];
  TEST_ASSERT_EQUAL_STRING("co_alarm", alertNames(m.state().alerts, buf, sizeof buf));
}

static void test_burner_without_call() {
  Model m;
  uint32_t t = 0;
  Inputs in;
  in.mvl = true;
  run(m, in, t, 4);
  TEST_ASSERT_EQUAL_UINT32(0, m.state().alerts & ALERT_BURNER_WITHOUT_CALL);
  run(m, in, t, 2);
  TEST_ASSERT_TRUE(m.state().alerts & ALERT_BURNER_WITHOUT_CALL);
}

static void test_flame_loss_and_short_cycle() {
  Model m;
  uint32_t t = 0;
  Inputs in;
  in.w1 = true;
  in.mvl = true;
  run(m, in, t, 30);
  TEST_ASSERT_TRUE(m.state().flame);
  in.mvl = false;  // valve drops while still calling
  m.update(in, t);
  TEST_ASSERT_TRUE(m.state().alerts & ALERT_FLAME_LOSS);
  TEST_ASSERT_TRUE(m.state().alerts & ALERT_SHORT_CYCLE);

  in.w1 = false;  // call ends: flame loss clears, short cycle stays latched
  t += 1000;
  m.update(in, t);
  TEST_ASSERT_EQUAL_UINT32(0, m.state().alerts & ALERT_FLAME_LOSS);
  TEST_ASSERT_TRUE(m.state().alerts & ALERT_SHORT_CYCLE);
}

static void test_low_delta_t_and_blower() {
  Model m;
  uint32_t t = 0;
  Inputs in;
  in.w1 = in.mvl = true;
  in.blowerAmps = 0.0f;
  in.supplyC = 25.0f;
  in.returnC = 20.0f;
  run(m, in, t, 301);
  TEST_ASSERT_TRUE(m.state().alerts & ALERT_LOW_DELTA_T);
  TEST_ASSERT_TRUE(m.state().alerts & ALERT_BLOWER_NOT_RUNNING);
}

static void test_board_lockout() {
  Model m;
  Inputs in;
  in.boardCode = 2;
  m.update(in, 0);
  TEST_ASSERT_EQUAL(Phase::Lockout, m.state().phase);
  TEST_ASSERT_TRUE(m.state().alerts & ALERT_BOARD_FAULT);
}

static void test_new_day_resets_counters() {
  Model m;
  m.restoreDay(5, 1200, 300);
  TEST_ASSERT_EQUAL_UINT32(5, m.state().cyclesToday);
  m.newDay();
  TEST_ASSERT_EQUAL_UINT32(0, m.state().cyclesToday);
  TEST_ASSERT_EQUAL_UINT32(0, m.state().burnerSecondsToday);
}

static void test_alert_names() {
  char buf[64];
  alertNames(ALERT_FLAME_LOSS | ALERT_BOARD_FAULT, buf, sizeof buf);
  TEST_ASSERT_EQUAL_STRING("flame_loss,board_fault", buf);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_idle_by_default);
  RUN_TEST(test_normal_two_stage_cycle);
  RUN_TEST(test_ignition_failure);
  RUN_TEST(test_ignition_failure_with_valve_trials);
  RUN_TEST(test_one_failed_trial_then_light);
  RUN_TEST(test_cycle_stats);
  RUN_TEST(test_rise_needs_settled_stage);
  RUN_TEST(test_filter_blower_time);
  RUN_TEST(test_co_alarm);
  RUN_TEST(test_burner_without_call);
  RUN_TEST(test_flame_loss_and_short_cycle);
  RUN_TEST(test_low_delta_t_and_blower);
  RUN_TEST(test_board_lockout);
  RUN_TEST(test_new_day_resets_counters);
  RUN_TEST(test_alert_names);
  return UNITY_END();
}
