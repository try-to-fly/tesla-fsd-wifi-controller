#include <unity.h>

#include "wifi_state_policy.h"

void test_expected_ap_stop_during_apply_is_ignored() {
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(APStopAction::IgnoreExpectedStop),
        static_cast<int>(decideAPStopAction(true, 2000, 0))
    );
}

void test_delayed_ap_stop_inside_settle_window_is_ignored() {
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(APStopAction::IgnoreExpectedStop),
        static_cast<int>(decideAPStopAction(false, 2200, 3000))
    );
}

void test_unexpected_ap_stop_after_settle_window_requests_recovery() {
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(APStopAction::RequestRecovery),
        static_cast<int>(decideAPStopAction(false, 3000, 3000))
    );
}

void test_unset_ap_settle_deadline_never_suppresses_recovery() {
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(APStopAction::RequestRecovery),
        static_cast<int>(decideAPStopAction(false, 0x90000000U, 0))
    );
}

void test_scan_waits_for_completion_event_before_framework_timeout_polling() {
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(UpstreamScanAction::Wait),
        static_cast<int>(decideUpstreamScanAction(false, false, 7000, 1000, 15000))
    );
}

void test_successful_scan_consumes_visible_results() {
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(UpstreamScanAction::ConsumeResults),
        static_cast<int>(decideUpstreamScanAction(true, true, 8000, 1000, 15000))
    );
    TEST_ASSERT_EQUAL_INT(2, chooseUpstreamNetworkIndex(2, 4));
}

void test_completion_event_wins_when_it_arrives_on_timeout_boundary() {
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(UpstreamScanAction::ConsumeResults),
        static_cast<int>(decideUpstreamScanAction(true, true, 16000, 1000, 15000))
    );
}

void test_failed_scan_uses_saved_network_fallback() {
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(UpstreamScanAction::UseSavedFallback),
        static_cast<int>(decideUpstreamScanAction(true, false, 8000, 1000, 15000))
    );
    TEST_ASSERT_EQUAL_INT(4, chooseUpstreamNetworkIndex(-1, 4));
}

void test_scan_timeout_uses_saved_network_fallback() {
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(UpstreamScanAction::UseSavedFallback),
        static_cast<int>(decideUpstreamScanAction(false, false, 16000, 1000, 15000))
    );
}

void test_no_saved_network_keeps_invalid_fallback() {
    TEST_ASSERT_EQUAL_INT(-1, chooseUpstreamNetworkIndex(-1, -1));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_expected_ap_stop_during_apply_is_ignored);
    RUN_TEST(test_delayed_ap_stop_inside_settle_window_is_ignored);
    RUN_TEST(test_unexpected_ap_stop_after_settle_window_requests_recovery);
    RUN_TEST(test_unset_ap_settle_deadline_never_suppresses_recovery);
    RUN_TEST(test_scan_waits_for_completion_event_before_framework_timeout_polling);
    RUN_TEST(test_successful_scan_consumes_visible_results);
    RUN_TEST(test_completion_event_wins_when_it_arrives_on_timeout_boundary);
    RUN_TEST(test_failed_scan_uses_saved_network_fallback);
    RUN_TEST(test_scan_timeout_uses_saved_network_fallback);
    RUN_TEST(test_no_saved_network_keeps_invalid_fallback);
    return UNITY_END();
}
