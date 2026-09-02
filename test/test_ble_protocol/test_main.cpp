#include <unity.h>

#include <string>
#include <vector>

#include "ble_protocol.h"

using namespace ble_protocol;

std::vector<uint8_t> frame(uint16_t id, uint16_t index, uint8_t flags, const char* payload) {
    std::string text(payload);
    std::vector<uint8_t> result(kHeaderSize + text.size());
    writeHeader(result.data(), FrameHeader{kVersion, flags, id, index});
    std::copy(text.begin(), text.end(), result.begin() + kHeaderSize);
    return result;
}

void test_reassembles_ordered_chunks() {
    Assembler assembler;
    std::string error;
    auto first = frame(7, 0, kFlagStart, "hello ");
    auto second = frame(7, 1, kFlagEnd, "world");

    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(AssembleResult::Partial),
        static_cast<int>(assembler.push(first.data(), first.size(), 100, 64, error))
    );
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(AssembleResult::Complete),
        static_cast<int>(assembler.push(second.data(), second.size(), 200, 64, error))
    );
    TEST_ASSERT_EQUAL_UINT16(7, assembler.messageId());
    TEST_ASSERT_EQUAL_STRING(
        "hello world",
        std::string(assembler.payload().begin(), assembler.payload().end()).c_str()
    );
}

void test_rejects_out_of_order_chunk() {
    Assembler assembler;
    std::string error;
    auto first = frame(1, 0, kFlagStart, "a");
    auto third = frame(1, 2, kFlagEnd, "b");
    assembler.push(first.data(), first.size(), 0, 64, error);

    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(AssembleResult::Error),
        static_cast<int>(assembler.push(third.data(), third.size(), 1, 64, error))
    );
    TEST_ASSERT_EQUAL_STRING("out-of-order", error.c_str());
}

void test_rejects_oversized_message() {
    Assembler assembler;
    std::string error;
    auto oversized = frame(2, 0, kFlagStart | kFlagEnd, "12345");

    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(AssembleResult::Error),
        static_cast<int>(assembler.push(oversized.data(), oversized.size(), 0, 4, error))
    );
    TEST_ASSERT_EQUAL_STRING("message-too-large", error.c_str());
}

void test_timeout_requires_new_start() {
    Assembler assembler;
    std::string error;
    auto first = frame(3, 0, kFlagStart, "a");
    auto second = frame(3, 1, kFlagEnd, "b");
    assembler.push(first.data(), first.size(), 0, 64, error);

    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(AssembleResult::Error),
        static_cast<int>(assembler.push(second.data(), second.size(), kAssemblyTimeoutMs + 1, 64, error))
    );
    TEST_ASSERT_EQUAL_STRING("missing-start", error.c_str());
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_reassembles_ordered_chunks);
    RUN_TEST(test_rejects_out_of_order_chunk);
    RUN_TEST(test_rejects_oversized_message);
    RUN_TEST(test_timeout_requires_new_start);
    return UNITY_END();
}
