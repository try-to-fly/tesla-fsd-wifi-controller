#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ble_protocol {

constexpr uint8_t kVersion = 1;
constexpr uint8_t kFlagStart = 1 << 0;
constexpr uint8_t kFlagEnd = 1 << 1;
constexpr uint8_t kFlagError = 1 << 2;
constexpr size_t kHeaderSize = 6;
constexpr size_t kMaxRequestBytes = 4096;
constexpr size_t kMaxResponseBytes = 24 * 1024;
constexpr uint32_t kAssemblyTimeoutMs = 5000;

enum class AssembleResult : uint8_t {
    Partial,
    Complete,
    Error,
};

struct FrameHeader {
    uint8_t version = kVersion;
    uint8_t flags = 0;
    uint16_t messageId = 0;
    uint16_t chunkIndex = 0;

    FrameHeader() = default;
    FrameHeader(uint8_t versionValue, uint8_t flagsValue, uint16_t messageIdValue, uint16_t chunkIndexValue)
        : version(versionValue), flags(flagsValue), messageId(messageIdValue), chunkIndex(chunkIndexValue) {}
};

inline void writeHeader(uint8_t* output, const FrameHeader& header) {
    output[0] = header.version;
    output[1] = header.flags;
    output[2] = static_cast<uint8_t>(header.messageId & 0xFF);
    output[3] = static_cast<uint8_t>(header.messageId >> 8);
    output[4] = static_cast<uint8_t>(header.chunkIndex & 0xFF);
    output[5] = static_cast<uint8_t>(header.chunkIndex >> 8);
}

inline bool readHeader(const uint8_t* data, size_t length, FrameHeader& header) {
    if (data == nullptr || length < kHeaderSize) return false;
    header.version = data[0];
    header.flags = data[1];
    header.messageId = static_cast<uint16_t>(data[2])
        | (static_cast<uint16_t>(data[3]) << 8);
    header.chunkIndex = static_cast<uint16_t>(data[4])
        | (static_cast<uint16_t>(data[5]) << 8);
    return true;
}

class Assembler {
public:
    AssembleResult push(
        const uint8_t* data,
        size_t length,
        uint32_t nowMs,
        size_t maxBytes,
        std::string& error
    ) {
        if (active_ && nowMs - lastActivityMs_ > kAssemblyTimeoutMs) reset();

        FrameHeader header;
        if (!readHeader(data, length, header)) return fail("frame-too-short", error);
        if (header.version != kVersion) return fail("unsupported-version", error);

        bool starts = (header.flags & kFlagStart) != 0;
        bool ends = (header.flags & kFlagEnd) != 0;
        if (starts) {
            if (header.chunkIndex != 0) return fail("invalid-start-index", error);
            reset();
            active_ = true;
            messageId_ = header.messageId;
        } else if (!active_) {
            return fail("missing-start", error);
        }

        if (header.messageId != messageId_ || header.chunkIndex != expectedChunk_) {
            return fail("out-of-order", error);
        }

        size_t payloadLength = length - kHeaderSize;
        if (payload_.size() + payloadLength > maxBytes) return fail("message-too-large", error);
        payload_.insert(payload_.end(), data + kHeaderSize, data + length);
        ++expectedChunk_;
        lastActivityMs_ = nowMs;

        if (!ends) return AssembleResult::Partial;
        active_ = false;
        return AssembleResult::Complete;
    }

    uint16_t messageId() const { return messageId_; }
    const std::vector<uint8_t>& payload() const { return payload_; }

    void reset() {
        active_ = false;
        messageId_ = 0;
        expectedChunk_ = 0;
        lastActivityMs_ = 0;
        payload_.clear();
    }

private:
    AssembleResult fail(const char* message, std::string& error) {
        error = message;
        reset();
        return AssembleResult::Error;
    }

    bool active_ = false;
    uint16_t messageId_ = 0;
    uint16_t expectedChunk_ = 0;
    uint32_t lastActivityMs_ = 0;
    std::vector<uint8_t> payload_;
};

}  // namespace ble_protocol
