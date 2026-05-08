#include <unity.h>

#include "speed_limit_policy.h"

void test_target_speed_table_matches_requested_policy() {
    TEST_ASSERT_EQUAL_UINT16(30, computeSmartTargetSpeedKph(20));
    TEST_ASSERT_EQUAL_UINT16(45, computeSmartTargetSpeedKph(30));
    TEST_ASSERT_EQUAL_UINT16(45, computeSmartTargetSpeedKph(35));
    TEST_ASSERT_EQUAL_UINT16(55, computeSmartTargetSpeedKph(40));
    TEST_ASSERT_EQUAL_UINT16(65, computeSmartTargetSpeedKph(50));
    TEST_ASSERT_EQUAL_UINT16(72, computeSmartTargetSpeedKph(60));
    TEST_ASSERT_EQUAL_UINT16(80, computeSmartTargetSpeedKph(70));
    TEST_ASSERT_EQUAL_UINT16(90, computeSmartTargetSpeedKph(80));
    TEST_ASSERT_EQUAL_UINT16(100, computeSmartTargetSpeedKph(90));
    TEST_ASSERT_EQUAL_UINT16(110, computeSmartTargetSpeedKph(100));
    TEST_ASSERT_EQUAL_UINT16(120, computeSmartTargetSpeedKph(110));
    TEST_ASSERT_EQUAL_UINT16(130, computeSmartTargetSpeedKph(120));
    TEST_ASSERT_EQUAL_UINT16(130, computeSmartTargetSpeedKph(130));
    TEST_ASSERT_EQUAL_UINT16(130, computeSmartTargetSpeedKph(140));
}

void test_key_speed_decisions_match_policy_table() {
    TEST_ASSERT_EQUAL_UINT8(10, computeSmartOffsetKph(20));
    TEST_ASSERT_EQUAL_UINT8(15, computeSmartOffsetKph(30));
    TEST_ASSERT_EQUAL_UINT8(15, computeSmartOffsetKph(40));
    TEST_ASSERT_EQUAL_UINT8(15, computeSmartOffsetKph(50));
    TEST_ASSERT_EQUAL_UINT8(12, computeSmartOffsetKph(60));
    TEST_ASSERT_EQUAL_UINT8(10, computeSmartOffsetKph(70));
    TEST_ASSERT_EQUAL_UINT8(10, computeSmartOffsetKph(80));
    TEST_ASSERT_EQUAL_UINT8(10, computeSmartOffsetKph(90));
    TEST_ASSERT_EQUAL_UINT8(10, computeSmartOffsetKph(100));
    TEST_ASSERT_EQUAL_UINT8(10, computeSmartOffsetKph(110));
    TEST_ASSERT_EQUAL_UINT8(10, computeSmartOffsetKph(120));
    TEST_ASSERT_EQUAL_UINT8(0, computeSmartOffsetKph(130));
}

void test_zero_or_invalid_speed_returns_zero_offset() {
    SmartSpeedDecision zero = computeSmartSpeedDecision(0);
    TEST_ASSERT_EQUAL_UINT16(0, zero.targetKph);
    TEST_ASSERT_EQUAL_UINT8(0, zero.offsetKph);
    TEST_ASSERT_EQUAL_UINT8(0, zero.offsetRaw);
    TEST_ASSERT_EQUAL_UINT8(0, computeSmartOffsetKph(0));
}

void test_km_only_helpers_follow_raw_can_values() {
    TEST_ASSERT_EQUAL_UINT16(100, decodeFiveStepSpeedLimitKph(20));
    TEST_ASSERT_EQUAL_UINT16(50, decodeMapSpeedLimitKph(10));
    TEST_ASSERT_EQUAL_INT(50, encodeOffsetFieldFromPercent(10));
    TEST_ASSERT_EQUAL_INT(150, encodeOffsetFieldFromPercent(30));
    TEST_ASSERT_EQUAL_INT(250, encodeOffsetFieldFromPercent(50));
    TEST_ASSERT_EQUAL_INT(255, encodeOffsetFieldFromPercent(60));
    TEST_ASSERT_EQUAL_INT(250, encodeOffsetFieldFromTargetKph(30, 45));
    TEST_ASSERT_EQUAL_INT(188, encodeOffsetFieldFromTargetKph(40, 55));
    TEST_ASSERT_EQUAL_INT(150, encodeOffsetFieldFromTargetKph(50, 65));
    TEST_ASSERT_EQUAL_INT(100, encodeOffsetFieldFromTargetKph(60, 72));
    TEST_ASSERT_EQUAL_INT(71, encodeOffsetFieldFromTargetKph(70, 80));
    TEST_ASSERT_EQUAL_INT(63, encodeOffsetFieldFromTargetKph(80, 90));
}

void test_smart_decision_keeps_target_offset_and_raw_separate() {
    SmartSpeedDecision decision = computeSmartSpeedDecision(60);
    TEST_ASSERT_EQUAL_UINT16(72, decision.targetKph);
    TEST_ASSERT_EQUAL_UINT8(12, decision.offsetKph);
    TEST_ASSERT_EQUAL_UINT8(100, decision.offsetRaw);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_target_speed_table_matches_requested_policy);
    RUN_TEST(test_key_speed_decisions_match_policy_table);
    RUN_TEST(test_zero_or_invalid_speed_returns_zero_offset);
    RUN_TEST(test_km_only_helpers_follow_raw_can_values);
    RUN_TEST(test_smart_decision_keeps_target_offset_and_raw_separate);
    return UNITY_END();
}
