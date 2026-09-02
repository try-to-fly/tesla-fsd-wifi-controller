import CryptoKit
import Foundation

enum BLEProtocolError: LocalizedError, Equatable {
    case frameTooShort
    case unsupportedVersion
    case invalidStartIndex
    case missingStart
    case outOfOrder
    case messageTooLarge
    case invalidTelemetry

    var errorDescription: String? {
        switch self {
        case .frameTooShort: "BLE 分片头不完整"
        case .unsupportedVersion: "ESP 使用了不支持的 BLE 协议版本"
        case .invalidStartIndex: "BLE 起始分片序号无效"
        case .missingStart: "BLE 响应缺少起始分片"
        case .outOfOrder: "BLE 响应分片乱序"
        case .messageTooLarge: "BLE 消息超过大小限制"
        case .invalidTelemetry: "BLE 遥测数据无效"
        }
    }
}

enum BLEResponseError: LocalizedError, Equatable {
    case invalidEnvelope
    case idMismatch
    case remote(code: String, message: String)

    var errorDescription: String? {
        switch self {
        case .invalidEnvelope: "ESP 返回了无法识别的响应"
        case .idMismatch: "ESP 响应与当前请求不匹配"
        case .remote(let code, let message): "ESP 错误（\(code)）：\(message)"
        }
    }
}

enum BLEEnvelope {
    static func decode(_ response: Data, expectedMessageID: UInt16) throws -> Data {
        let object = try JSONSerialization.jsonObject(with: response)
        guard let envelope = object as? [String: Any],
              let responseID = envelope["id"] as? NSNumber,
              let ok = envelope["ok"] as? Bool else { throw BLEResponseError.invalidEnvelope }
        guard responseID.uint16Value == expectedMessageID else { throw BLEResponseError.idMismatch }
        if !ok {
            let remote = envelope["error"] as? [String: Any]
            throw BLEResponseError.remote(
                code: remote?["code"] as? String ?? "unknown",
                message: remote?["message"] as? String ?? "ESP 未返回错误详情"
            )
        }
        return try JSONSerialization.data(
            withJSONObject: envelope["data"] ?? NSNull(),
            options: [.fragmentsAllowed]
        )
    }
}

struct BLEFrameHeader: Equatable {
    static let size = 6
    static let version: UInt8 = 1
    static let start: UInt8 = 1 << 0
    static let end: UInt8 = 1 << 1
    static let error: UInt8 = 1 << 2

    let flags: UInt8
    let messageID: UInt16
    let chunkIndex: UInt16

    init(flags: UInt8, messageID: UInt16, chunkIndex: UInt16) {
        self.flags = flags
        self.messageID = messageID
        self.chunkIndex = chunkIndex
    }

    init(data: Data) throws {
        guard data.count >= Self.size else { throw BLEProtocolError.frameTooShort }
        guard data[0] == Self.version else { throw BLEProtocolError.unsupportedVersion }
        flags = data[1]
        messageID = UInt16(data[2]) | UInt16(data[3]) << 8
        chunkIndex = UInt16(data[4]) | UInt16(data[5]) << 8
    }

    var data: Data {
        Data([
            Self.version,
            flags,
            UInt8(messageID & 0xff),
            UInt8(messageID >> 8),
            UInt8(chunkIndex & 0xff),
            UInt8(chunkIndex >> 8),
        ])
    }
}

struct BLEMessageAssembler {
    private(set) var messageID: UInt16?
    private var expectedChunk: UInt16 = 0
    private var payload = Data()
    private var lastActivity: TimeInterval = 0
    private var active = false

    mutating func push(
        _ frame: Data,
        now: TimeInterval = ProcessInfo.processInfo.systemUptime,
        maximumBytes: Int = 24 * 1024
    ) throws -> Data? {
        if active, now - lastActivity > 5 { reset() }
        let header = try BLEFrameHeader(data: frame)
        let starts = header.flags & BLEFrameHeader.start != 0
        let ends = header.flags & BLEFrameHeader.end != 0

        if starts {
            guard header.chunkIndex == 0 else { throw fail(.invalidStartIndex) }
            reset()
            active = true
            messageID = header.messageID
        } else if !active {
            throw fail(.missingStart)
        }

        guard header.messageID == messageID, header.chunkIndex == expectedChunk else {
            throw fail(.outOfOrder)
        }
        let chunk = frame.dropFirst(BLEFrameHeader.size)
        guard payload.count + chunk.count <= maximumBytes else {
            throw fail(.messageTooLarge)
        }
        payload.append(chunk)
        expectedChunk &+= 1
        lastActivity = now
        guard ends else { return nil }
        active = false
        return payload
    }

    mutating func reset() {
        active = false
        messageID = nil
        expectedChunk = 0
        lastActivity = 0
        payload.removeAll(keepingCapacity: true)
    }

    private mutating func fail(_ error: BLEProtocolError) -> BLEProtocolError {
        reset()
        return error
    }
}

enum BLEProtocolV1 {
    static let serviceUUID = "F5D00001-8B1A-4E7A-9C2D-7A0D5E1C0001"
    static let commandUUID = "F5D00002-8B1A-4E7A-9C2D-7A0D5E1C0001"
    static let responseUUID = "F5D00003-8B1A-4E7A-9C2D-7A0D5E1C0001"
    static let telemetryUUID = "F5D00004-8B1A-4E7A-9C2D-7A0D5E1C0001"
    static let maximumRequestBytes = 4 * 1024

    static func pairingCode(for accessPointPassword: String) -> String {
        let digest = SHA256.hash(data: Data(accessPointPassword.utf8))
        let bytes = Array(digest)
        let value = UInt32(bytes[0]) << 24
            | UInt32(bytes[1]) << 16
            | UInt32(bytes[2]) << 8
            | UInt32(bytes[3])
        return String(format: "%06u", value % 1_000_000)
    }

    static func timeout(for operation: String) -> TimeInterval {
        operation == "upstream.scan" ? 25 : 5
    }

    static func frames(messageID: UInt16, payload: Data, maximumWriteLength: Int) throws -> [Data] {
        guard payload.count <= maximumRequestBytes else { throw BLEProtocolError.messageTooLarge }
        let chunkSize = max(1, maximumWriteLength - BLEFrameHeader.size)
        var result: [Data] = []
        var offset = 0
        var index: UInt16 = 0

        repeat {
            let length = min(chunkSize, payload.count - offset)
            var flags: UInt8 = offset == 0 ? BLEFrameHeader.start : 0
            if offset + length >= payload.count { flags |= BLEFrameHeader.end }
            var frame = BLEFrameHeader(flags: flags, messageID: messageID, chunkIndex: index).data
            if length > 0 { frame.append(payload[offset..<(offset + length)]) }
            result.append(frame)
            offset += length
            index &+= 1
        } while offset < payload.count
        return result
    }
}

struct BLETelemetry: Equatable {
    let canReady: Bool
    let canActive: Bool
    let fsdTriggered: Bool
    let fsdEnabled: Bool
    let accessPointRunning: Bool
    let upstreamConnected: Bool
    let natEnabled: Bool
    let thermalProtection: Bool
    let vehicleSpeedValid: Bool
    let automaticProfile: Bool
    let dnsEnabled: Bool
    let hardwareMode: Int
    let speedProfile: Int
    let vehicleSpeedKPH: Double
    let detectedSpeedLimitKPH: Int
    let appliedSpeedOffsetKPH: Int
    let chipTemperatureC: Double?
    let upstreamRSSI: Int
    let accessPointClients: Int
    let receivedDelta: Int
    let modifiedDelta: Int
    let errorDelta: Int
    let sequence: UInt8

    init(data: Data) throws {
        guard data.count == 20, data[0] == BLEFrameHeader.version else {
            throw BLEProtocolError.invalidTelemetry
        }
        let flags = UInt16(data[1]) | UInt16(data[2]) << 8
        func enabled(_ bit: Int) -> Bool { flags & (1 << bit) != 0 }
        func uint16(_ offset: Int) -> UInt16 { UInt16(data[offset]) | UInt16(data[offset + 1]) << 8 }

        canReady = enabled(0)
        canActive = enabled(1)
        fsdTriggered = enabled(2)
        fsdEnabled = enabled(3)
        accessPointRunning = enabled(4)
        upstreamConnected = enabled(5)
        natEnabled = enabled(6)
        thermalProtection = enabled(7)
        vehicleSpeedValid = enabled(8)
        automaticProfile = enabled(9)
        dnsEnabled = enabled(10)
        hardwareMode = Int(data[3])
        speedProfile = Int(data[4])
        vehicleSpeedKPH = Double(uint16(5)) / 100
        detectedSpeedLimitKPH = Int(data[7])
        appliedSpeedOffsetKPH = Int(Int8(bitPattern: data[8]))
        let rawTemperature = Int16(bitPattern: uint16(9))
        chipTemperatureC = rawTemperature == .min ? nil : Double(rawTemperature) / 10
        upstreamRSSI = Int(Int8(bitPattern: data[11]))
        accessPointClients = Int(data[12])
        receivedDelta = Int(uint16(13))
        modifiedDelta = Int(uint16(15))
        errorDelta = Int(data[17])
        sequence = data[18]
    }
}

enum ReconnectPolicy {
    static func delay(for attempt: Int) -> TimeInterval {
        min(pow(2, Double(max(0, attempt))), 10)
    }
}
