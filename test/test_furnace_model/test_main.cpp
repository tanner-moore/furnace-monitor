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
  RUN_TEST(test_burner_without_call);
  RUN_TEST(test_flame_loss_and_short_cycle);
  RUN_TEST(test_low_delta_t_and_blower);
  RUN_TEST(test_board_lockout);
  RUN_TEST(test_new_day_resets_counters);
  RUN_TEST(test_alert_names);
  return UNITY_END();
}
