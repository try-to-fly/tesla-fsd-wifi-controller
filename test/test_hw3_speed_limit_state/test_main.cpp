#include <algorithm>
#include <vector>

#include <unity.h>

#include "handlers.h"

uint32_t fakeMillisValue = 0;

uint32_t millis() {
    return fakeMillisValue;
}

namespace {

struct FakeCanDriver : public CanDriver {
    std::vector<CanFrame> sentFrames;
    bool sendShouldSucceed = true;

    bool init() override { return true; }
    bool send(const CanFrame& frame) override {
        if (!sendShouldSucceed) return false;
        sentFrames.push_back(frame);
        return true;
    }
    bool read(CanFrame&) override { return false; }
    void setFilters(const uint32_t*, uint8_t) override {}
};

CanFrame makeHW3VisionFrame(uint8_t fusedRaw, uint8_t visionRaw) {
    CanFrame frame;
    frame.id = 921;
    frame.data[1] = fusedRaw & 0x1F;
    frame.data[2] = visionRaw & 0x1F;
    return frame;
}

CanFrame makeHW3MapFrame(uint8_t mapRaw) {
    CanFrame frame;
    frame.id = 760;
    frame.data[6] = mapRaw & 0x1F;
    return frame;
}

CanFrame makeHW3Index0Frame(bool fsdSelected, uint8_t userOffsetKph) {
    CanFrame frame;
    frame.id = 1021;
    frame.data[0] = 0;
    frame.data[3] = static_cast<uint8_t>((std::min<uint8_t>(userOffsetKph, 33) + 30) << 1);
    frame.data[4] = fsdSelected ? 0x40 : 0x00;
    return frame;
}

CanFrame makeHW3Index2Frame() {
    CanFrame frame;
    frame.id = 1021;
    frame.data[0] = 2;
    return frame;
}

void writeUnsignedLittleEndian(CanFrame& frame, uint8_t startBit, uint8_t bitLength, uint32_t value) {
    for (uint8_t i = 0; i < bitLength; ++i) {
        uint8_t absoluteBit = static_cast<uint8_t>(startBit + i);
        uint8_t byteIndex = absoluteBit / 8U;
        uint8_t bitIndex = absoluteBit % 8U;
        uint8_t mask = static_cast<uint8_t>(1U << bitIndex);
        if ((value >> i) & 0x01U) {
            frame.data[byteIndex] |= mask;
        } else {
            frame.data[byteIndex] &= static_cast<uint8_t>(~mask);
        }
    }
}

CanFrame makeESPVehicleSpeedFrame(uint16_t rawSpeed, bool valid = true) {
    CanFrame frame;
    frame.id = ESP_VEHICLE_SPEED_ID;
    writeUnsignedLittleEndian(frame, 40, 1, valid ? 1U : 0U);
    writeUnsignedLittleEndian(frame, 42, 10, rawSpeed);
    return frame;
}

CanFrame makeDIVehicleSpeedFrame(uint16_t rawSpeed) {
    CanFrame frame;
    frame.id = DI_VEHICLE_SPEED_ID;
    writeUnsignedLittleEndian(frame, 12, 12, rawSpeed);
    return frame;
}

uint16_t decodeInjectedOffsetField(const CanFrame& frame) {
    return static_cast<uint16_t>(((frame.data[0] >> 6) & 0x03) | ((frame.data[1] & 0x3F) << 2));
}

void resetRuntimeConfig() {
    cfg.fsdEnable = true;
    cfg.hwMode = 1;
    cfg.speedProfile = 1;
    cfg.profileModeAuto = true;
    cfg.isaChimeSuppress = false;
    cfg.emergencyDetection = true;
    cfg.chinaMode = true;
    cfg.detectedSpeedLimitKph = 0;
    cfg.detectedSpeedSource = static_cast<uint8_t>(SpeedLimitSource::None);
    cfg.appliedSpeedOffsetKph = 0;
    cfg.vehicleSpeedCentiKph = 0;
    cfg.vehicleSpeedSource = static_cast<uint8_t>(VehicleSpeedSource::None);
    cfg.lastVehicleSpeedMillis = 0;
    cfg.rxCount = 0;
    cfg.modifiedCount = 0;
    cfg.errorCount = 0;
    cfg.lastRxMillis = 0;
    cfg.lastModifiedMillis = 0;
    cfg.canOK = false;
    cfg.fsdTriggered = false;
    cfg.uptimeStart = 0;
    fakeMillisValue = 0;
}

}  // namespace

void setUp() {
    resetRuntimeConfig();
    resetHW3SpeedLimitState();
    resetVehicleSpeedState();
}

void tearDown() {}

void test_hw3_can_resolve_limit_before_760_arrives() {
    FakeCanDriver driver;
    CanFrame frame = makeHW3VisionFrame(20, 18);

    handleHW3(frame, driver);

    TEST_ASSERT_EQUAL_UINT16(100, cfg.detectedSpeedLimitKph);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(SpeedLimitSource::Fused), cfg.detectedSpeedSource);
}

void test_hw3_reset_clears_cached_speed_limit_state() {
    FakeCanDriver driver;
    CanFrame visionFrame = makeHW3VisionFrame(20, 18);
    CanFrame mapFrame = makeHW3MapFrame(10);
    CanFrame index0Frame = makeHW3Index0Frame(true, 12);
    handleHW3(visionFrame, driver);
    handleHW3(mapFrame, driver);
    handleHW3(index0Frame, driver);
    cfg.appliedSpeedOffsetKph = 10;

    resetHW3SpeedLimitState();

    TEST_ASSERT_EQUAL_UINT8(0, hw3RawUserOffsetKph);
    TEST_ASSERT_EQUAL_UINT16(0, hw3VisionSpeedLimitKph);
    TEST_ASSERT_EQUAL_UINT16(0, hw3FusedSpeedLimitKph);
    TEST_ASSERT_EQUAL_UINT16(0, hw3MapSpeedLimitKph);
    TEST_ASSERT_EQUAL_UINT16(0, cfg.detectedSpeedLimitKph);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(SpeedLimitSource::None), cfg.detectedSpeedSource);
    TEST_ASSERT_EQUAL_UINT8(0, cfg.appliedSpeedOffsetKph);
}

void test_hw3_applies_smart_offset_when_fsd_is_active() {
    FakeCanDriver driver;
    cfg.chinaMode = false;

    CanFrame visionFrame = makeHW3VisionFrame(20, 18);
    CanFrame index0Frame = makeHW3Index0Frame(true, 8);
    CanFrame index2Frame = makeHW3Index2Frame();
    handleHW3(visionFrame, driver);
    handleHW3(index0Frame, driver);
    handleHW3(index2Frame, driver);

    TEST_ASSERT_TRUE(cfg.fsdTriggered);
    TEST_ASSERT_EQUAL_UINT8(10, cfg.appliedSpeedOffsetKph);
    TEST_ASSERT_EQUAL_UINT(2, static_cast<unsigned int>(driver.sentFrames.size()));
    TEST_ASSERT_EQUAL_UINT16(50, decodeInjectedOffsetField(driver.sentFrames.back()));
}

void test_hw3_injects_percent_field_for_smart_limit_policy() {
    FakeCanDriver driver;
    cfg.chinaMode = false;

    CanFrame visionFrame = makeHW3VisionFrame(12, 0);
    CanFrame index0Frame = makeHW3Index0Frame(true, 8);
    CanFrame index2Frame = makeHW3Index2Frame();
    handleHW3(visionFrame, driver);
    handleHW3(index0Frame, driver);
    handleHW3(index2Frame, driver);

    TEST_ASSERT_EQUAL_UINT16(60, cfg.detectedSpeedLimitKph);
    TEST_ASSERT_EQUAL_UINT8(11, cfg.appliedSpeedOffsetKph);
    TEST_ASSERT_EQUAL_UINT16(90, decodeInjectedOffsetField(driver.sentFrames.back()));
}

void test_hw3_injects_percent_field_for_low_speed_limit_policy() {
    FakeCanDriver driver;
    cfg.chinaMode = false;

    CanFrame visionFrame = makeHW3VisionFrame(6, 0);
    CanFrame index0Frame = makeHW3Index0Frame(true, 8);
    CanFrame index2Frame = makeHW3Index2Frame();
    handleHW3(visionFrame, driver);
    handleHW3(index0Frame, driver);
    handleHW3(index2Frame, driver);

    TEST_ASSERT_EQUAL_UINT16(30, cfg.detectedSpeedLimitKph);
    TEST_ASSERT_EQUAL_UINT8(6, cfg.appliedSpeedOffsetKph);
    TEST_ASSERT_EQUAL_UINT16(100, decodeInjectedOffsetField(driver.sentFrames.back()));
}

void test_hw3_does_not_inject_offset_when_fsd_is_inactive() {
    FakeCanDriver driver;
    cfg.chinaMode = false;
    cfg.appliedSpeedOffsetKph = 9;

    CanFrame visionFrame = makeHW3VisionFrame(20, 18);
    CanFrame index0Frame = makeHW3Index0Frame(false, 12);
    CanFrame index2Frame = makeHW3Index2Frame();
    handleHW3(visionFrame, driver);
    handleHW3(index0Frame, driver);
    driver.sentFrames.clear();
    handleHW3(index2Frame, driver);

    TEST_ASSERT_FALSE(cfg.fsdTriggered);
    TEST_ASSERT_EQUAL_UINT8(0, cfg.appliedSpeedOffsetKph);
    TEST_ASSERT_TRUE(driver.sentFrames.empty());
}

void test_handle_message_tracks_rx_count_without_timestamp_work() {
    FakeCanDriver driver;
    CanFrame frame = makeHW3VisionFrame(20, 18);

    fakeMillisValue = 1234;
    handleMessage(frame, driver);

    TEST_ASSERT_EQUAL_UINT32(1, cfg.rxCount);
    TEST_ASSERT_EQUAL_UINT32(0, cfg.lastRxMillis);
}

void test_hw3_send_success_tracks_last_modified_millis() {
    FakeCanDriver driver;
    CanFrame index0Frame = makeHW3Index0Frame(true, 12);

    fakeMillisValue = 5678;
    handleHW3(index0Frame, driver);

    TEST_ASSERT_EQUAL_UINT32(1, cfg.modifiedCount);
    TEST_ASSERT_EQUAL_UINT32(5678, cfg.lastModifiedMillis);
    TEST_ASSERT_EQUAL_UINT32(0, cfg.errorCount);
}

void test_hw3_send_failure_increments_error_without_updating_modified_millis() {
    FakeCanDriver driver;
    driver.sendShouldSucceed = false;
    CanFrame index0Frame = makeHW3Index0Frame(true, 12);

    fakeMillisValue = 91011;
    handleHW3(index0Frame, driver);

    TEST_ASSERT_EQUAL_UINT32(0, cfg.modifiedCount);
    TEST_ASSERT_EQUAL_UINT32(1, cfg.errorCount);
    TEST_ASSERT_EQUAL_UINT32(0, cfg.lastModifiedMillis);
}

void test_handle_message_uses_esp_vehicle_speed_when_available() {
    FakeCanDriver driver;
    CanFrame frame = makeESPVehicleSpeedFrame(135);

    fakeMillisValue = 2200;
    handleMessage(frame, driver);

    TEST_ASSERT_EQUAL_UINT16(6750, cfg.vehicleSpeedCentiKph);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(VehicleSpeedSource::ESP), cfg.vehicleSpeedSource);
    TEST_ASSERT_EQUAL_UINT32(2200, cfg.lastVehicleSpeedMillis);
}

void test_handle_message_falls_back_to_di_vehicle_speed() {
    FakeCanDriver driver;
    CanFrame frame = makeDIVehicleSpeedFrame(1250);

    fakeMillisValue = 3300;
    handleMessage(frame, driver);

    TEST_ASSERT_EQUAL_UINT16(6000, cfg.vehicleSpeedCentiKph);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(VehicleSpeedSource::DI), cfg.vehicleSpeedSource);
    TEST_ASSERT_EQUAL_UINT32(3300, cfg.lastVehicleSpeedMillis);
}

void test_di_vehicle_speed_does_not_override_fresh_esp_source() {
    FakeCanDriver driver;
    CanFrame espFrame = makeESPVehicleSpeedFrame(140);
    CanFrame diFrame = makeDIVehicleSpeedFrame(1250);

    fakeMillisValue = 4000;
    handleMessage(espFrame, driver);
    fakeMillisValue = 4200;
    handleMessage(diFrame, driver);

    TEST_ASSERT_EQUAL_UINT16(7000, cfg.vehicleSpeedCentiKph);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(VehicleSpeedSource::ESP), cfg.vehicleSpeedSource);
    TEST_ASSERT_EQUAL_UINT32(4000, cfg.lastVehicleSpeedMillis);
}

void test_invalid_esp_vehicle_speed_immediately_falls_back_to_fresh_di() {
    FakeCanDriver driver;
    CanFrame diFrame = makeDIVehicleSpeedFrame(1250);
    CanFrame espFrame = makeESPVehicleSpeedFrame(140);
    CanFrame invalidEspFrame = makeESPVehicleSpeedFrame(0, false);

    fakeMillisValue = 5000;
    handleMessage(diFrame, driver);
    fakeMillisValue = 5100;
    handleMessage(espFrame, driver);
    fakeMillisValue = 5200;
    handleMessage(invalidEspFrame, driver);

    TEST_ASSERT_EQUAL_UINT16(6000, cfg.vehicleSpeedCentiKph);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(VehicleSpeedSource::DI), cfg.vehicleSpeedSource);
    TEST_ASSERT_EQUAL_UINT32(5000, cfg.lastVehicleSpeedMillis);
}

void test_invalid_esp_vehicle_speed_clears_telemetry_without_di_fallback() {
    FakeCanDriver driver;
    CanFrame espFrame = makeESPVehicleSpeedFrame(140);
    CanFrame invalidEspFrame = makeESPVehicleSpeedFrame(0, false);

    fakeMillisValue = 6000;
    handleMessage(espFrame, driver);
    fakeMillisValue = 6100;
    handleMessage(invalidEspFrame, driver);

    TEST_ASSERT_EQUAL_UINT16(0, cfg.vehicleSpeedCentiKph);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(VehicleSpeedSource::None), cfg.vehicleSpeedSource);
    TEST_ASSERT_EQUAL_UINT32(0, cfg.lastVehicleSpeedMillis);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_hw3_can_resolve_limit_before_760_arrives);
    RUN_TEST(test_hw3_reset_clears_cached_speed_limit_state);
    RUN_TEST(test_hw3_applies_smart_offset_when_fsd_is_active);
    RUN_TEST(test_hw3_injects_percent_field_for_smart_limit_policy);
    RUN_TEST(test_hw3_injects_percent_field_for_low_speed_limit_policy);
    RUN_TEST(test_hw3_does_not_inject_offset_when_fsd_is_inactive);
    RUN_TEST(test_handle_message_tracks_rx_count_without_timestamp_work);
    RUN_TEST(test_hw3_send_success_tracks_last_modified_millis);
    RUN_TEST(test_hw3_send_failure_increments_error_without_updating_modified_millis);
    RUN_TEST(test_handle_message_uses_esp_vehicle_speed_when_available);
    RUN_TEST(test_handle_message_falls_back_to_di_vehicle_speed);
    RUN_TEST(test_di_vehicle_speed_does_not_override_fresh_esp_source);
    RUN_TEST(test_invalid_esp_vehicle_speed_immediately_falls_back_to_fresh_di);
    RUN_TEST(test_invalid_esp_vehicle_speed_clears_telemetry_without_di_fallback);
    return UNITY_END();
}
