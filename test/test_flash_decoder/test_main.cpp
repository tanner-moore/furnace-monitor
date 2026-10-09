#include <unity.h>

#include "flash_decoder.h"

using namespace furnace;

void setUp() {}
void tearDown() {}

// Plays a pattern of (lit, durationMs) at 50 ms steps.
struct Player {
  FlashDecoder& d;
  uint32_t t = 0;
  void hold(bool lit, uint32_t ms) {
    for (uint32_t end = t + ms; t < end; t += 50) d.sample(lit, t);
  }
  void group(int flashes) {
    for (int i = 0; i < flashes; i++) {
      hold(true, 250);
      hold(false, 250);
    }
    hold(false, 2000);
  }
};

static void test_fault_code_needs_two_matching_groups() {
  FlashDecoder d;
  Player p{d};
  p.hold(false, 500);
  p.group(3);
  TEST_ASSERT_EQUAL_INT(0, d.code());
  p.group(3);
  TEST_ASSERT_EQUAL_INT(3, d.code());
  TEST_ASSERT_EQUAL_STRING("pressure switch problem", FlashDecoder::describe(d.code()));
}

static void test_heartbeat_is_normal() {
  FlashDecoder d;
  Player p{d};
  p.hold(false, 500);
  p.group(4);
  p.group(4);
  TEST_ASSERT_EQUAL_INT(4, d.code());
  // Continuous fast blink with no long gaps.
  for (int i = 0; i < 20; i++) {
    p.hold(true, 200);
    p.hold(false, 200);
  }
  TEST_ASSERT_EQUAL_INT(0, d.code());
}

static void test_steady_on() {
  FlashDecoder d;
  Player p{d};
  p.hold(false, 500);
  p.hold(true, 6000);
  TEST_ASSERT_EQUAL_INT(FlashDecoder::kCodeSteadyOn, d.code());
}

static void test_goes_stale() {
  FlashDecoder d;
  Player p{d};
  p.hold(false, 500);
  p.group(7);
  p.group(7);
  TEST_ASSERT_EQUAL_INT(7, d.code());
  p.hold(false, 25000);
  TEST_ASSERT_EQUAL_INT(0, d.code());
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_fault_code_needs_two_matching_groups);
  RUN_TEST(test_heartbeat_is_normal);
  RUN_TEST(test_steady_on);
  RUN_TEST(test_goes_stale);
  return UNITY_END();
}
