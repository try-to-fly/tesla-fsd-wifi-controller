#include <unity.h>

#include "speed_limit_policy.h"

void test_percent_caps_follow_expected_bands() {
    TEST_ASSERT_EQUAL_UINT8(20, getSmartOffsetPercentCap(30));
    TEST_ASSERT_EQUAL_UINT8(20, getSmartOffsetPercentCap(40));
    TEST_ASSERT_EQUAL_UINT8(18, getSmartOffsetPercentCap(60));
    TEST_ASSERT_EQUAL_UINT8(15, getSmartOffsetPercentCap(80));
    TEST_ASSERT_EQUAL_UINT8(12, getSmartOffsetPercentCap(90));
    TEST_ASSERT_EQUAL_UINT8(10, getSmartOffsetPercentCap(100));
    TEST_ASSERT_EQUAL_UINT8(10, getSmartOffsetPercentCap(120));
}

void test_hard_caps_follow_expected_bands() {
    TEST_ASSERT_EQUAL_UINT8(8, getSmartOffsetHardCapKph(30));
    TEST_ASSERT_EQUAL_UINT8(10, getSmartOffsetHardCapKph(40));
    TEST_ASSERT_EQUAL_UINT8(12, getSmartOffsetHardCapKph(60));
    TEST_ASSERT_EQUAL_UINT8(15, getSmartOffsetHardCapKph(80));
    TEST_ASSERT_EQUAL_UINT8(15, getSmartOffsetHardCapKph(120));
}

void test_key_speed_decisions_match_policy_table() {
    TEST_ASSERT_EQUAL_UINT8(6, computeSmartOffsetKph(30));
    TEST_ASSERT_EQUAL_UINT8(8, computeSmartOffsetKph(40));
    TEST_ASSERT_EQUAL_UINT8(9, computeSmartOffsetKph(50));
    TEST_ASSERT_EQUAL_UINT8(11, computeSmartOffsetKph(60));
    TEST_ASSERT_EQUAL_UINT8(11, computeSmartOffsetKph(70));
    TEST_ASSERT_EQUAL_UINT8(12, computeSmartOffsetKph(80));
    TEST_ASSERT_EQUAL_UINT8(11, computeSmartOffsetKph(90));
    TEST_ASSERT_EQUAL_UINT8(10, computeSmartOffsetKph(100));
    TEST_ASSERT_EQUAL_UINT8(11, computeSmartOffsetKph(110));
    TEST_ASSERT_EQUAL_UINT8(12, computeSmartOffsetKph(120));
}

void test_zero_or_invalid_speed_returns_zero_offset() {
    SmartSpeedDecision zero = computeSmartSpeedDecision(0);
    TEST_ASSERT_EQUAL_UINT8(0, zero.percentCap);
    TEST_ASSERT_EQUAL_UINT8(0, zero.hardCapKph);
    TEST_ASSERT_EQUAL_UINT8(0, zero.offsetKph);
    TEST_ASSERT_EQUAL_UINT8(0, computeSmartOffsetKph(0));
}

void test_km_only_helpers_follow_raw_can_values() {
    TEST_ASSERT_EQUAL_UINT16(100, decodeFiveStepSpeedLimitKph(20));
    TEST_ASSERT_EQUAL_UINT16(50, decodeMapSpeedLimitKph(10));
    TEST_ASSERT_EQUAL_INT(50, encodeOffsetFieldFromPercent(10));
    TEST_ASSERT_EQUAL_INT(90, encodeOffsetFieldFromPercent(18));
    TEST_ASSERT_EQUAL_INT(100, encodeOffsetFieldFromPercent(20));
    TEST_ASSERT_EQUAL_INT(100, encodeOffsetFieldFromPercent(30));
}

void test_smart_decision_keeps_percent_and_kph_separate() {
    SmartSpeedDecision decision = computeSmartSpeedDecision(60);
    TEST_ASSERT_EQUAL_UINT8(18, decision.percentCap);
    TEST_ASSERT_EQUAL_UINT8(12, decision.hardCapKph);
    TEST_ASSERT_EQUAL_UINT8(11, decision.offsetKph);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_percent_caps_follow_expected_bands);
    RUN_TEST(test_hard_caps_follow_expected_bands);
    RUN_TEST(test_key_speed_decisions_match_policy_table);
    RUN_TEST(test_zero_or_invalid_speed_returns_zero_offset);
    RUN_TEST(test_km_only_helpers_follow_raw_can_values);
    RUN_TEST(test_smart_decision_keeps_percent_and_kph_separate);
    return UNITY_END();
}
