import Foundation
import XCTest
@testable import FSDController

final class BLEProtocolTests: XCTestCase {
    func testFragmentationAndReassembly() throws {
        let payload = Data(String(repeating: "x", count: 420).utf8)
        let frames = try BLEProtocolV1.frames(messageID: 7, payload: payload, maximumWriteLength: 64)
        XCTAssertGreaterThan(frames.count, 1)

        var assembler = BLEMessageAssembler()
        var result: Data?
        for frame in frames { result = try assembler.push(frame) }
        XCTAssertEqual(result, payload)
        XCTAssertEqual(assembler.messageID, 7)
    }

    func testRejectsOutOfOrderAndOversizedResponse() throws {
        let payload = Data("hello".utf8)
        let frames = try BLEProtocolV1.frames(messageID: 2, payload: payload, maximumWriteLength: 8)
        var assembler = BLEMessageAssembler()
        XCTAssertNil(try assembler.push(frames[0]))
        XCTAssertThrowsError(try assembler.push(frames[2])) { error in
            XCTAssertEqual(error as? BLEProtocolError, .outOfOrder)
        }

        let oneFrame = try BLEProtocolV1.frames(messageID: 3, payload: payload, maximumWriteLength: 64)[0]
        var smallAssembler = BLEMessageAssembler()
        XCTAssertThrowsError(try smallAssembler.push(oneFrame, maximumBytes: 4)) { error in
            XCTAssertEqual(error as? BLEProtocolError, .messageTooLarge)
        }
    }

    func testResponseCorrelationAndRemoteError() throws {
        let success = Data(#"{"id":9,"ok":true,"data":{"value":1}}"#.utf8)
        XCTAssertThrowsError(try BLEEnvelope.decode(success, expectedMessageID: 8)) { error in
            XCTAssertEqual(error as? BLEResponseError, .idMismatch)
        }
        let failure = Data(#"{"id":9,"ok":false,"error":{"code":"invalid-config","message":"密码太短"}}"#.utf8)
        XCTAssertThrowsError(try BLEEnvelope.decode(failure, expectedMessageID: 9)) { error in
            XCTAssertEqual(
                error as? BLEResponseError,
                .remote(code: "invalid-config", message: "密码太短")
            )
            XCTAssertTrue(error.localizedDescription.contains("密码太短"))
        }
    }

    func testTimeoutPairingCodeAndReconnectPolicy() {
        XCTAssertEqual(BLEProtocolV1.timeout(for: "status.get"), 5)
        XCTAssertEqual(BLEProtocolV1.timeout(for: "upstream.scan"), 25)
        XCTAssertEqual(BLEProtocolV1.pairingCode(for: "12345678"), "716353")
        XCTAssertEqual(ReconnectPolicy.delay(for: 0), 1)
        XCTAssertEqual(ReconnectPolicy.delay(for: 4), 10)
    }

    func testTelemetryAndStatusMapping() throws {
        var packet = Data(repeating: 0, count: 20)
        packet[0] = 1
        packet[1] = 0b0111_1011
        packet[2] = 0b0000_0101
        packet[3] = 1
        packet[4] = 3
        packet[5] = 0x39
        packet[6] = 0x30
        packet[7] = 80
        packet[8] = UInt8(bitPattern: -5)
        packet[9] = 0xA7
        packet[10] = 0x01
        packet[11] = UInt8(bitPattern: -48)
        packet[12] = 1
        packet[13] = 2
        packet[15] = 1
        packet[17] = 3
        packet[18] = 4
        let telemetry = try BLETelemetry(data: packet)
        XCTAssertTrue(telemetry.canReady)
        XCTAssertTrue(telemetry.fsdEnabled)
        XCTAssertEqual(telemetry.vehicleSpeedKPH, 123.45, accuracy: 0.001)
        XCTAssertEqual(telemetry.appliedSpeedOffsetKPH, -5)
        XCTAssertEqual(telemetry.chipTemperatureC, 42.3)

        let statusJSON = Data(#"{"rx":10,"modified":3,"errors":1,"fsdEnable":1,"hwMode":2,"speedProfile":4,"profileMode":1,"canOK":true,"apSSID":"FSD-Controller","upstreamNetworks":[],"dnsBlockedRequests":[],"minFreeHeap":65536}"#.utf8)
        let status = try JSONDecoder().decode(ControllerStatus.self, from: statusJSON)
        XCTAssertEqual(status.rx, 10)
        XCTAssertEqual(status.fsdEnable, 1)
        XCTAssertEqual(status.hwMode, 2)
        XCTAssertEqual(status.minFreeHeap, 65_536)
    }
}
