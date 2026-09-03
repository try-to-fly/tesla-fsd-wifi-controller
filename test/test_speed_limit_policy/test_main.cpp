#include <unity.h>

#include "speed_limit_policy.h"

void test_target_speed_table_matches_requested_policy() {
    TEST_ASSERT_EQUAL_UINT16(30, computeSmartTargetSpeedKph(20));
    TEST_ASSERT_EQUAL_UINT16(45, computeSmartTargetSpeedKph(30));
    TEST_ASSERT_EQUAL_UINT16(39, computeSmartTargetSpeedKph(35));
    TEST_ASSERT_EQUAL_UINT16(55, computeSmartTargetSpeedKph(40));
    TEST_ASSERT_EQUAL_UINT16(65, computeSmartTargetSpeedKph(50));
    TEST_ASSERT_EQUAL_UINT16(72, computeSmartTargetSpeedKph(60));
    TEST_ASSERT_EQUAL_UINT16(72, computeSmartTargetSpeedKph(65));
    TEST_ASSERT_EQUAL_UINT16(77, computeSmartTargetSpeedKph(70));
    TEST_ASSERT_EQUAL_UINT16(88, computeSmartTargetSpeedKph(80));
    TEST_ASSERT_EQUAL_UINT16(99, computeSmartTargetSpeedKph(90));
    TEST_ASSERT_EQUAL_UINT16(110, computeSmartTargetSpeedKph(100));
    TEST_ASSERT_EQUAL_UINT16(121, computeSmartTargetSpeedKph(110));
    TEST_ASSERT_EQUAL_UINT16(132, computeSmartTargetSpeedKph(120));
    TEST_ASSERT_EQUAL_UINT16(143, computeSmartTargetSpeedKph(130));
    TEST_ASSERT_EQUAL_UINT16(154, computeSmartTargetSpeedKph(140));
}

void test_key_speed_decisions_match_policy_table() {
    TEST_ASSERT_EQUAL_UINT8(10, computeSmartOffsetKph(20));
    TEST_ASSERT_EQUAL_UINT8(15, computeSmartOffsetKph(30));
    TEST_ASSERT_EQUAL_UINT8(15, computeSmartOffsetKph(40));
    TEST_ASSERT_EQUAL_UINT8(15, computeSmartOffsetKph(50));
    TEST_ASSERT_EQUAL_UINT8(12, computeSmartOffsetKph(60));
    TEST_ASSERT_EQUAL_UINT8(7, computeSmartOffsetKph(65));
    TEST_ASSERT_EQUAL_UINT8(7, computeSmartOffsetKph(70));
    TEST_ASSERT_EQUAL_UINT8(8, computeSmartOffsetKph(80));
    TEST_ASSERT_EQUAL_UINT8(9, computeSmartOffsetKph(90));
    TEST_ASSERT_EQUAL_UINT8(10, computeSmartOffsetKph(100));
    TEST_ASSERT_EQUAL_UINT8(11, computeSmartOffsetKph(110));
    TEST_ASSERT_EQUAL_UINT8(12, computeSmartOffsetKph(120));
    // 130 智能 uncapped 目标 143，整体上限 135 → 偏移 5
    TEST_ASSERT_EQUAL_UINT8(5, computeSmartOffsetKph(130));
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
    TEST_ASSERT_EQUAL_INT(50, encodeOffsetFieldFromTargetKph(70, 77));
    TEST_ASSERT_EQUAL_INT(50, encodeOffsetFieldFromTargetKph(80, 88));
}

void test_smart_decision_keeps_target_offset_and_raw_separate() {
    SmartSpeedDecision decision = computeSmartSpeedDecision(60);
    TEST_ASSERT_EQUAL_UINT16(72, decision.targetKph);
    TEST_ASSERT_EQUAL_UINT8(12, decision.offsetKph);
    TEST_ASSERT_EQUAL_UINT8(100, decision.offsetRaw);

    SmartSpeedDecision highSpeedDecision = computeSmartSpeedDecision(70);
    TEST_ASSERT_EQUAL_UINT16(77, highSpeedDecision.targetKph);
    TEST_ASSERT_EQUAL_UINT8(7, highSpeedDecision.offsetKph);
    TEST_ASSERT_EQUAL_UINT8(50, highSpeedDecision.offsetRaw);
}

void test_percent_policies_and_overall_cap() {
    SmartSpeedDecision smart120 = computeSpeedDecision(120, SPEED_LIMIT_POLICY_SMART);
    TEST_ASSERT_EQUAL_UINT16(132, smart120.targetKph);
    TEST_ASSERT_EQUAL_UINT8(12, smart120.offsetKph);
    TEST_ASSERT_EQUAL_UINT8(50, smart120.offsetRaw);

    SmartSpeedDecision zero120 = computeSpeedDecision(120, 0);
    TEST_ASSERT_EQUAL_UINT16(120, zero120.targetKph);
    TEST_ASSERT_EQUAL_UINT8(0, zero120.offsetKph);
    TEST_ASSERT_EQUAL_UINT8(0, zero120.offsetRaw);

    SmartSpeedDecision five120 = computeSpeedDecision(120, 5);
    TEST_ASSERT_EQUAL_UINT16(126, five120.targetKph);
    TEST_ASSERT_EQUAL_UINT8(6, five120.offsetKph);
    TEST_ASSERT_EQUAL_UINT8(25, five120.offsetRaw);

    SmartSpeedDecision ten120 = computeSpeedDecision(120, 10);
    TEST_ASSERT_EQUAL_UINT16(132, ten120.targetKph);
    TEST_ASSERT_EQUAL_UINT8(12, ten120.offsetKph);
    TEST_ASSERT_EQUAL_UINT8(50, ten120.offsetRaw);

    SmartSpeedDecision cappedSmart = computeSpeedDecision(130, SPEED_LIMIT_POLICY_SMART);
    TEST_ASSERT_EQUAL_UINT16(135, cappedSmart.targetKph);
    TEST_ASSERT_EQUAL_UINT8(5, cappedSmart.offsetKph);
    TEST_ASSERT_EQUAL_UINT8(encodeOffsetFieldFromTargetKph(130, 135), cappedSmart.offsetRaw);

    SmartSpeedDecision overLimit = computeSpeedDecision(140, 10);
    TEST_ASSERT_EQUAL_UINT16(135, overLimit.targetKph);
    TEST_ASSERT_EQUAL_UINT8(0, overLimit.offsetKph);
    TEST_ASSERT_EQUAL_UINT8(0, overLimit.offsetRaw);

    TEST_ASSERT_FALSE(isValidSpeedLimitPolicy(7));
    TEST_ASSERT_TRUE(isValidSpeedLimitPolicy(SPEED_LIMIT_POLICY_SMART));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_target_speed_table_matches_requested_policy);
    RUN_TEST(test_key_speed_decisions_match_policy_table);
    RUN_TEST(test_zero_or_invalid_speed_returns_zero_offset);
    RUN_TEST(test_km_only_helpers_follow_raw_can_values);
    RUN_TEST(test_smart_decision_keeps_target_offset_and_raw_separate);
    RUN_TEST(test_percent_policies_and_overall_cap);
    return UNITY_END();
}
