@preconcurrency import CoreBluetooth
import Combine
import Foundation
import Security

enum ControllerConnectionState: Equatable {
    case waitingForBluetooth
    case scanning
    case connecting(String)
    case pairing(String)
    case connected(String)
    case disconnected(String)
    case unavailable(String)

    var title: String {
        switch self {
        case .waitingForBluetooth: "等待蓝牙"
        case .scanning: "正在查找控制器"
        case .connecting(let name): "正在连接 \(name)"
        case .pairing(let name): "正在安全配对 \(name)"
        case .connected(let name): "已连接 \(name)"
        case .disconnected(let reason): "已断开：\(reason)"
        case .unavailable(let reason): reason
        }
    }

    var isConnected: Bool {
        if case .connected = self { return true }
        return false
    }
}

struct DiscoveredController: Identifiable {
    let peripheral: CBPeripheral
    let rssi: Int
    var id: UUID { peripheral.identifier }
    var name: String { peripheral.name ?? "FSD Controller" }
}

enum ControllerError: LocalizedError {
    case bluetooth(String)
    case noConnection
    case requestBusy
    case disconnected(String)
    case timedOut(String)

    var errorDescription: String? {
        switch self {
        case .bluetooth(let reason): reason
        case .noConnection: "尚未连接 ESP 控制器"
        case .requestBusy: "已有操作正在执行，请等待完成"
        case .disconnected(let reason): "ESP 连接已断开：\(reason)"
        case .timedOut(let operation): "操作 \(operation) 等待 ESP 响应超时"
        }
    }
}

@MainActor
final class BLEController: NSObject, ObservableObject {
    @Published private(set) var connectionState: ControllerConnectionState = .waitingForBluetooth
    @Published private(set) var discoveredControllers: [DiscoveredController] = []
    @Published private(set) var status: ControllerStatus?
    @Published private(set) var telemetry: BLETelemetry?
    @Published private(set) var scannedNetworks: [ScannedUpstreamNetwork] = []
    @Published private(set) var diagnosticLog = ""
    @Published private(set) var isBusy = false
    @Published private(set) var hasPreferredController = StoredPeripheral.load() != nil
    @Published var lastError: String?

    private let serviceUUID = CBUUID(string: BLEProtocolV1.serviceUUID)
    private let commandUUID = CBUUID(string: BLEProtocolV1.commandUUID)
    private let responseUUID = CBUUID(string: BLEProtocolV1.responseUUID)
    private let telemetryUUID = CBUUID(string: BLEProtocolV1.telemetryUUID)
    private var central: CBCentralManager!
    private var peripheral: CBPeripheral?
    private var commandCharacteristic: CBCharacteristic?
    private var responseCharacteristic: CBCharacteristic?
    private var telemetryCharacteristic: CBCharacteristic?
    private var pending: PendingRequest?
    private var nextMessageID: UInt16 = 1
    private var reconnectAttempt = 0
    private var reconnectTask: Task<Void, Never>?
    private var connectionTimeoutTask: Task<Void, Never>?
    private var shouldReconnect = true
    private var telemetryTicks = 0

    override init() {
        super.init()
        central = CBCentralManager(delegate: self, queue: .main)
    }

    func startScanning() {
        guard central.state == .poweredOn else { return }
        shouldReconnect = true
        discoveredControllers.removeAll()
        connectionState = .scanning
        central.scanForPeripherals(withServices: [serviceUUID], options: [
            CBCentralManagerScanOptionAllowDuplicatesKey: false,
        ])
    }

    func select(_ controller: DiscoveredController) {
        shouldReconnect = true
        StoredPeripheral.save(controller.id)
        hasPreferredController = true
        connect(to: controller.peripheral)
    }

    func forgetController() {
        shouldReconnect = false
        reconnectTask?.cancel()
        StoredPeripheral.clear()
        hasPreferredController = false
        if let peripheral { central.cancelPeripheralConnection(peripheral) }
        resetConnectionData()
        startScanning()
    }

    func refreshStatus(silent: Bool = false) async {
        await run(operation: "status.get", silent: silent)
    }

    func setConfig(_ values: [String: Any]) async {
        await run(operation: "config.set", arguments: values)
    }

    func scanUpstream() async {
        await run(operation: "upstream.scan")
    }

    func saveUpstream(ssid: String, password: String?) async {
        var args: [String: Any] = ["ssid": ssid]
        if let password { args["pass"] = password }
        await run(operation: "upstream.save", arguments: args)
    }

    func deleteUpstream(ssid: String) async {
        await run(operation: "upstream.delete", arguments: ["ssid": ssid])
    }

    func clearBlockedDNS() async {
        await run(operation: "dns.blocked.clear")
    }

    func readDebugLog() async {
        await run(operation: "debug.read")
    }

    func clearDebugLog() async {
        await run(operation: "debug.clear")
    }

    private func run(operation: String, arguments: [String: Any]? = nil, silent: Bool = false) async {
        do {
            _ = try await perform(operation: operation, arguments: arguments)
            if !silent { lastError = nil }
        } catch {
            if !silent || connectionState.isConnected {
                lastError = error.localizedDescription
            }
        }
    }

    private func perform(operation: String, arguments: [String: Any]?) async throws -> Data {
        guard connectionState.isConnected,
              let peripheral,
              let commandCharacteristic else { throw ControllerError.noConnection }
        guard pending == nil else { throw ControllerError.requestBusy }

        let messageID = nextMessageID
        nextMessageID = nextMessageID == .max ? 1 : nextMessageID + 1
        var object: [String: Any] = ["id": Int(messageID), "op": operation]
        if let arguments { object["args"] = arguments }
        let payload = try JSONSerialization.data(withJSONObject: object)
        let frames = try BLEProtocolV1.frames(
            messageID: messageID,
            payload: payload,
            maximumWriteLength: peripheral.maximumWriteValueLength(for: .withResponse)
        )

        return try await withCheckedThrowingContinuation { continuation in
            let request = PendingRequest(
                id: messageID,
                operation: operation,
                frames: frames,
                continuation: continuation
            )
            pending = request
            isBusy = true
            request.timeoutTask = Task { [weak self] in
                try? await Task.sleep(for: .seconds(BLEProtocolV1.timeout(for: operation)))
                guard !Task.isCancelled else { return }
                self?.timeoutRequest(messageID)
            }
            writeNextFrame(commandCharacteristic)
        }
    }

    private func writeNextFrame(_ characteristic: CBCharacteristic) {
        guard let pending, pending.nextFrameIndex < pending.frames.count, let peripheral else { return }
        peripheral.writeValue(pending.frames[pending.nextFrameIndex], for: characteristic, type: .withResponse)
    }

    private func timeoutRequest(_ messageID: UInt16) {
        guard pending?.id == messageID else { return }
        failPending(ControllerError.timedOut(pending?.operation ?? "unknown"))
    }

    private func finishPending(with response: Data) {
        guard let pending else { return }
        do {
            let data = try BLEEnvelope.decode(response, expectedMessageID: pending.id)
            try applyResponse(data, for: pending.operation)
            pending.timeoutTask?.cancel()
            pending.continuation.resume(returning: data)
            self.pending = nil
            isBusy = false
        } catch {
            failPending(error)
        }
    }

    private func applyResponse(_ data: Data, for operation: String) throws {
        switch operation {
        case "status.get", "config.set", "upstream.save", "upstream.delete", "dns.blocked.clear":
            status = try JSONDecoder().decode(ControllerStatus.self, from: data)
        case "upstream.scan":
            scannedNetworks = try JSONDecoder().decode(ScanResponse.self, from: data).results
        case "debug.read":
            diagnosticLog = try JSONDecoder().decode(LogResponse.self, from: data).text
        case "debug.clear":
            diagnosticLog = ""
        default:
            break
        }
    }

    private func failPending(_ error: Error) {
        guard let pending else { return }
        pending.timeoutTask?.cancel()
        pending.continuation.resume(throwing: error)
        self.pending = nil
        isBusy = false
    }

    private func connect(to peripheral: CBPeripheral) {
        central.stopScan()
        reconnectTask?.cancel()
        connectionTimeoutTask?.cancel()
        self.peripheral = peripheral
        peripheral.delegate = self
        connectionState = .connecting(peripheral.name ?? "FSD Controller")
        central.connect(peripheral)
        connectionTimeoutTask = Task { [weak self, weak peripheral] in
            try? await Task.sleep(for: .seconds(10))
            guard !Task.isCancelled, let self, let peripheral,
                  peripheral.state != .connected else { return }
            self.central.cancelPeripheralConnection(peripheral)
            self.lastError = "连接 ESP 超时，请确认控制器已启动且在附近"
            self.scheduleReconnect()
        }
    }

    private func restoreOrScan() {
        if let id = StoredPeripheral.load(),
           let saved = central.retrievePeripherals(withIdentifiers: [id]).first {
            connect(to: saved)
        } else {
            startScanning()
        }
    }

    private func scheduleReconnect(reason: String? = nil) {
        guard shouldReconnect, StoredPeripheral.load() != nil, central.state == .poweredOn else { return }
        reconnectTask?.cancel()
        if let reason { connectionState = .disconnected(reason) }
        let delay = ReconnectPolicy.delay(for: reconnectAttempt)
        reconnectAttempt += 1
        reconnectTask = Task { [weak self] in
            try? await Task.sleep(for: .seconds(delay))
            guard !Task.isCancelled else { return }
            self?.restoreOrScan()
        }
    }

    private func resetConnectionData() {
        commandCharacteristic = nil
        responseCharacteristic = nil
        telemetryCharacteristic = nil
        peripheral = nil
        status = nil
        telemetry = nil
        scannedNetworks = []
        diagnosticLog = ""
    }

    private func markReadyIfPossible() {
        guard let peripheral,
              responseCharacteristic?.isNotifying == true,
              telemetryCharacteristic?.isNotifying == true,
              !connectionState.isConnected else { return }
        reconnectAttempt = 0
        connectionState = .connected(peripheral.name ?? "FSD Controller")
        Task { await refreshStatus() }
    }
}

extension BLEController: @preconcurrency CBCentralManagerDelegate {
    func centralManagerDidUpdateState(_ central: CBCentralManager) {
        switch central.state {
        case .poweredOn:
            restoreOrScan()
        case .poweredOff:
            failPending(ControllerError.bluetooth("iPhone 蓝牙已关闭"))
            connectionState = .unavailable("iPhone 蓝牙已关闭")
        case .unauthorized:
            failPending(ControllerError.bluetooth("FSD Controller 没有蓝牙权限"))
            connectionState = .unavailable("请在系统设置中允许蓝牙权限")
        case .unsupported:
            connectionState = .unavailable("当前设备不支持蓝牙低功耗")
        case .resetting:
            connectionState = .waitingForBluetooth
        case .unknown:
            connectionState = .waitingForBluetooth
        @unknown default:
            connectionState = .unavailable("未知蓝牙状态")
        }
    }

    func centralManager(
        _ central: CBCentralManager,
        didDiscover peripheral: CBPeripheral,
        advertisementData: [String: Any],
        rssi RSSI: NSNumber
    ) {
        guard (peripheral.name ?? "").hasPrefix("FSD-Controller-") else { return }
        let item = DiscoveredController(peripheral: peripheral, rssi: RSSI.intValue)
        if let index = discoveredControllers.firstIndex(where: { $0.id == item.id }) {
            discoveredControllers[index] = item
        } else {
            discoveredControllers.append(item)
        }
    }

    func centralManager(_ central: CBCentralManager, didConnect peripheral: CBPeripheral) {
        connectionTimeoutTask?.cancel()
        connectionState = .pairing(peripheral.name ?? "FSD Controller")
        peripheral.discoverServices([serviceUUID])
    }

    func centralManager(_ central: CBCentralManager, didFailToConnect peripheral: CBPeripheral, error: Error?) {
        let reason = error?.localizedDescription ?? "连接失败"
        lastError = "无法连接 ESP：\(reason)"
        scheduleReconnect(reason: reason)
    }

    func centralManager(_ central: CBCentralManager, didDisconnectPeripheral peripheral: CBPeripheral, error: Error?) {
        connectionTimeoutTask?.cancel()
        let reason = error?.localizedDescription ?? "设备主动断开或重启"
        failPending(ControllerError.disconnected(reason))
        commandCharacteristic = nil
        responseCharacteristic = nil
        telemetryCharacteristic = nil
        telemetry = nil
        scheduleReconnect(reason: reason)
    }
}

extension BLEController: @preconcurrency CBPeripheralDelegate {
    func peripheral(_ peripheral: CBPeripheral, didDiscoverServices error: Error?) {
        if let error {
            lastError = "读取 ESP 服务失败：\(error.localizedDescription)"
            central.cancelPeripheralConnection(peripheral)
            return
        }
        guard let service = peripheral.services?.first(where: { $0.uuid == serviceUUID }) else {
            lastError = "连接的设备没有 FSD 控制服务"
            central.cancelPeripheralConnection(peripheral)
            return
        }
        peripheral.discoverCharacteristics([commandUUID, responseUUID, telemetryUUID], for: service)
    }

    func peripheral(_ peripheral: CBPeripheral, didDiscoverCharacteristicsFor service: CBService, error: Error?) {
        if let error {
            lastError = "读取 ESP 特征失败：\(error.localizedDescription)"
            central.cancelPeripheralConnection(peripheral)
            return
        }
        for characteristic in service.characteristics ?? [] {
            switch characteristic.uuid {
            case commandUUID: commandCharacteristic = characteristic
            case responseUUID: responseCharacteristic = characteristic
            case telemetryUUID: telemetryCharacteristic = characteristic
            default: break
            }
        }
        guard commandCharacteristic != nil,
              let responseCharacteristic,
              let telemetryCharacteristic else {
            lastError = "ESP BLE 接口不完整，请升级控制器固件"
            central.cancelPeripheralConnection(peripheral)
            return
        }
        peripheral.setNotifyValue(true, for: responseCharacteristic)
        peripheral.setNotifyValue(true, for: telemetryCharacteristic)
    }

    func peripheral(_ peripheral: CBPeripheral, didUpdateNotificationStateFor characteristic: CBCharacteristic, error: Error?) {
        if let error {
            lastError = "安全配对或通知订阅失败：\(error.localizedDescription)"
            central.cancelPeripheralConnection(peripheral)
            return
        }
        markReadyIfPossible()
    }

    func peripheral(_ peripheral: CBPeripheral, didWriteValueFor characteristic: CBCharacteristic, error: Error?) {
        guard characteristic.uuid == commandUUID, let pending else { return }
        if let error {
            failPending(ControllerError.bluetooth("向 ESP 写入失败：\(error.localizedDescription)"))
            return
        }
        pending.nextFrameIndex += 1
        if pending.nextFrameIndex < pending.frames.count {
            writeNextFrame(characteristic)
        }
    }

    func peripheral(_ peripheral: CBPeripheral, didUpdateValueFor characteristic: CBCharacteristic, error: Error?) {
        if let error {
            lastError = "读取 ESP 数据失败：\(error.localizedDescription)"
            return
        }
        guard let data = characteristic.value else { return }
        if characteristic.uuid == telemetryUUID {
            do {
                telemetry = try BLETelemetry(data: data)
                telemetryTicks += 1
                if telemetryTicks % 5 == 0, pending == nil {
                    Task { await refreshStatus(silent: true) }
                }
            } catch {
                lastError = error.localizedDescription
            }
            return
        }
        guard characteristic.uuid == responseUUID, let pending else { return }
        do {
            if let response = try pending.assembler.push(data) {
                finishPending(with: response)
            }
        } catch {
            failPending(error)
        }
    }
}

private final class PendingRequest {
    let id: UInt16
    let operation: String
    let frames: [Data]
    let continuation: CheckedContinuation<Data, Error>
    var nextFrameIndex = 0
    var assembler = BLEMessageAssembler()
    var timeoutTask: Task<Void, Never>?

    init(
        id: UInt16,
        operation: String,
        frames: [Data],
        continuation: CheckedContinuation<Data, Error>
    ) {
        self.id = id
        self.operation = operation
        self.frames = frames
        self.continuation = continuation
    }
}

private struct ScanResponse: Decodable {
    let results: [ScannedUpstreamNetwork]
}

private struct LogResponse: Decodable {
    let text: String
}

private enum StoredPeripheral {
    private static let service = "com.trytofly.fsdcontroller"
    private static let account = "preferred-peripheral"

    static func save(_ id: UUID) {
        let value = Data(id.uuidString.utf8)
        let query: [String: Any] = [
            kSecClass as String: kSecClassGenericPassword,
            kSecAttrService as String: service,
            kSecAttrAccount as String: account,
        ]
        let attributes: [String: Any] = [
            kSecValueData as String: value,
            kSecAttrAccessible as String: kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly,
        ]
        if SecItemUpdate(query as CFDictionary, attributes as CFDictionary) == errSecItemNotFound {
            var item = query
            item.merge(attributes) { _, new in new }
            SecItemAdd(item as CFDictionary, nil)
        }
    }

    static func load() -> UUID? {
        let query: [String: Any] = [
            kSecClass as String: kSecClassGenericPassword,
            kSecAttrService as String: service,
            kSecAttrAccount as String: account,
            kSecReturnData as String: true,
            kSecMatchLimit as String: kSecMatchLimitOne,
        ]
        var result: CFTypeRef?
        guard SecItemCopyMatching(query as CFDictionary, &result) == errSecSuccess,
              let data = result as? Data,
              let value = String(data: data, encoding: .utf8) else { return nil }
        return UUID(uuidString: value)
    }

    static func clear() {
        SecItemDelete([
            kSecClass as String: kSecClassGenericPassword,
            kSecAttrService as String: service,
            kSecAttrAccount as String: account,
        ] as CFDictionary)
    }
}
