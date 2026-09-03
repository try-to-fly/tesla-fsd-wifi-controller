#include <unity.h>

#include "controller_contract.h"

using namespace controller_contract;

void test_config_validation_boundaries() {
    ConfigValidationInput input;
    input.hasHardwareMode = true;
    input.hardwareMode = 2;
    input.hasSpeedProfile = true;
    input.speedProfile = 4;
    input.touchesAP = true;
    input.apSSIDLength = 32;
    input.apPasswordLength = 63;
    input.hasDNSAllowlist = true;
    input.dnsAllowlistLength = 31;
    input.dnsAllowlistCapacity = 32;
    input.hasDNSBlocklist = true;
    input.dnsBlocklistLength = 31;
    input.dnsBlocklistCapacity = 32;
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(ConfigValidationError::None),
        static_cast<int>(validateConfig(input))
    );

    input.apPasswordLength = 7;
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(ConfigValidationError::InvalidAPPassword),
        static_cast<int>(validateConfig(input))
    );

    input.apPasswordLength = 63;
    input.hasSpeedLimitPolicy = true;
    input.speedLimitPolicy = 255;
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(ConfigValidationError::None),
        static_cast<int>(validateConfig(input))
    );

    input.speedLimitPolicy = 7;
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(ConfigValidationError::InvalidSpeedLimitPolicy),
        static_cast<int>(validateConfig(input))
    );
}

void test_ble_status_hides_ap_password() {
    TEST_ASSERT_TRUE(includesAPPassword(StatusAudience::Web));
    TEST_ASSERT_FALSE(includesAPPassword(StatusAudience::BLE));
}

void test_operation_allowlist_is_fixed() {
    TEST_ASSERT_TRUE(isSupportedBLEOperation("status.get"));
    TEST_ASSERT_TRUE(isSupportedBLEOperation("debug.clear"));
    TEST_ASSERT_FALSE(isSupportedBLEOperation("ota.write"));
    TEST_ASSERT_FALSE(isSupportedBLEOperation(nullptr));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_config_validation_boundaries);
    RUN_TEST(test_ble_status_hides_ap_password);
    RUN_TEST(test_operation_allowlist_is_fixed);
    return UNITY_END();
}
