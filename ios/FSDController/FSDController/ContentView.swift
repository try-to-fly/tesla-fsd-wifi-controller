import SwiftUI

struct ContentView: View {
    @EnvironmentObject private var controller: BLEController
    @State private var showingDevicePicker = false

    var body: some View {
        TabView {
            NavigationStack { OverviewView() }
                .tabItem { Label("概览", systemImage: "gauge.with.dots.needle.50percent") }
            NavigationStack { FSDView() }
                .tabItem { Label("FSD", systemImage: "steeringwheel") }
            NavigationStack { NetworkView() }
                .tabItem { Label("网络", systemImage: "wifi.router") }
            NavigationStack { DNSView() }
                .tabItem { Label("DNS", systemImage: "shield.lefthalf.filled") }
            NavigationStack { DiagnosticsView() }
                .tabItem { Label("诊断", systemImage: "stethoscope") }
        }
        .safeAreaInset(edge: .top) {
            ConnectionBanner(showDevicePicker: $showingDevicePicker)
        }
        .fullScreenCover(isPresented: $showingDevicePicker) {
            PairingView(canCancel: controller.hasPreferredController) {
                showingDevicePicker = false
            }
            .environmentObject(controller)
        }
        .alert("操作失败", isPresented: Binding(
            get: { controller.lastError != nil },
            set: { if !$0 { controller.lastError = nil } }
        )) {
            Button("好") { controller.lastError = nil }
        } message: {
            Text(controller.lastError ?? "未知错误")
        }
        .onAppear {
            if !controller.hasPreferredController { showingDevicePicker = true }
        }
        .onChange(of: controller.hasPreferredController) { _, hasPreferred in
            if !hasPreferred { showingDevicePicker = true }
        }
        .onChange(of: controller.connectionState) { _, state in
            if state.isConnected { showingDevicePicker = false }
        }
    }
}

private struct ConnectionBanner: View {
    @EnvironmentObject private var controller: BLEController
    @Binding var showDevicePicker: Bool

    var body: some View {
        HStack(spacing: 10) {
            Circle()
                .fill(controller.connectionState.isConnected ? .green : .orange)
                .frame(width: 9, height: 9)
            Text(controller.connectionState.title)
                .font(.footnote)
                .lineLimit(1)
            Spacer()
            if controller.isBusy { ProgressView().controlSize(.small) }
            if controller.connectionState.isConnected {
                Button("刷新") { Task { await controller.refreshStatus() } }
                    .font(.footnote.weight(.semibold))
            } else {
                Button("设备") { showDevicePicker = true }
                    .font(.footnote.weight(.semibold))
            }
        }
        .padding(.horizontal)
        .padding(.vertical, 8)
        .background(.bar)
    }
}

private struct OverviewView: View {
    @EnvironmentObject private var controller: BLEController

    var body: some View {
        List {
            Section("实时状态") {
                LabeledContent("BLE", value: controller.connectionState.isConnected ? "已连接" : "未连接")
                LabeledContent("CAN", value: canActive ? "持续收包" : "无活动")
                LabeledContent("FSD", value: fsdEnabled ? (fsdTriggered ? "已触发" : "已启用") : "已关闭")
                LabeledContent("车辆速度", value: vehicleSpeedValid ? String(format: "%.1f km/h", vehicleSpeed) : "--")
                LabeledContent("识别限速", value: detectedLimit > 0 ? "\(detectedLimit) km/h" : "--")
                LabeledContent("当前偏移", value: "\(appliedOffset >= 0 ? "+" : "")\(appliedOffset) km/h")
                LabeledContent("限速策略", value: speedLimitPolicyLabel)
                LabeledContent("芯片温度", value: temperature.map { String(format: "%.1f ℃", $0) } ?? "--")
            }
            Section("网络链路") {
                LabeledContent("ESP 热点", value: apRunning ? "运行中" : "未运行")
                LabeledContent("上游热点", value: upstreamConnected ? "已连接" : "未连接")
                LabeledContent("NAT", value: natEnabled ? "已启用" : "未启用")
                LabeledContent("热点客户端", value: "\(controller.telemetry?.accessPointClients ?? controller.status?.apClients ?? 0)")
            }
        }
        .navigationTitle("FSD Controller")
        .refreshable { await controller.refreshStatus() }
    }

    private var canActive: Bool { controller.telemetry?.canActive ?? controller.status?.rxActive ?? false }
    private var fsdEnabled: Bool { controller.telemetry?.fsdEnabled ?? (controller.status?.fsdEnable == 1) }
    private var fsdTriggered: Bool { controller.telemetry?.fsdTriggered ?? controller.status?.fsdTriggered ?? false }
    private var vehicleSpeed: Double { controller.telemetry?.vehicleSpeedKPH ?? controller.status?.vehicleSpeedKph ?? 0 }
    private var vehicleSpeedValid: Bool { controller.telemetry?.vehicleSpeedValid ?? controller.status?.vehicleSpeedValid ?? false }
    private var detectedLimit: Int { controller.telemetry?.detectedSpeedLimitKPH ?? controller.status?.detectedSpeedLimitKph ?? 0 }
    private var appliedOffset: Int { controller.telemetry?.appliedSpeedOffsetKPH ?? controller.status?.appliedSpeedOffsetKph ?? 0 }
    private var speedLimitPolicyLabel: String {
        switch controller.status?.speedLimitPolicy ?? 255 {
        case 0: return "超速 0% · 整体上限 135 km/h"
        case 5: return "超速 5% · 整体上限 135 km/h"
        case 10: return "超速 10% · 整体上限 135 km/h"
        default: return "智能 · 整体上限 135 km/h"
        }
    }
    private var temperature: Double? { controller.telemetry?.chipTemperatureC ?? controller.status?.chipTempC }
    private var apRunning: Bool { controller.telemetry?.accessPointRunning ?? controller.status?.apRunning ?? false }
    private var upstreamConnected: Bool { controller.telemetry?.upstreamConnected ?? controller.status?.upstreamConnected ?? false }
    private var natEnabled: Bool { controller.telemetry?.natEnabled ?? (controller.status?.natEnabled == 1) }
}

private struct FSDView: View {
    @EnvironmentObject private var controller: BLEController

    var body: some View {
        Form {
            Section("控制") {
                Toggle("启用 FSD", isOn: boolBinding("fsdEnable") { $0.fsdEnable == 1 })
                Picker("硬件版本", selection: intBinding("hwMode") { $0.hwMode }) {
                    Text("LEGACY").tag(0)
                    Text("HW3").tag(1)
                    Text("HW4").tag(2)
                }
                Picker("速度模式", selection: intBinding("speedProfile") { $0.speedProfile }) {
                    Text("保守").tag(0)
                    Text("默认").tag(1)
                    Text("适中").tag(2)
                    Text("激进").tag(3)
                    Text("最大").tag(4)
                }
                Picker("模式来源", selection: boolBinding("profileMode") { $0.profileMode == 1 }) {
                    Text("手动").tag(false)
                    Text("自动（拨杆）").tag(true)
                }
                Picker("限速策略", selection: intBinding("speedLimitPolicy") { $0.speedLimitPolicy }) {
                    Text("智能").tag(255)
                    Text("超速 0%").tag(0)
                    Text("超速 5%").tag(5)
                    Text("超速 10%").tag(10)
                }
                Text("百分比模式按识别限速统一上浮；所有模式目标车速不超过 135 km/h。")
                    .font(.footnote)
                    .foregroundStyle(.secondary)
            }
            Section("附加功能") {
                Toggle("抑制 ISA 限速提示音", isOn: boolBinding("isaChime") { $0.isaChime == 1 })
                Toggle("紧急车辆检测", isOn: boolBinding("emergencyDet") { $0.emergencyDet == 1 })
                Toggle("中国模式", isOn: boolBinding("chinaMode") { $0.chinaMode == 1 })
            }
        }
        .navigationTitle("FSD")
        .disabled(!controller.connectionState.isConnected || controller.isBusy || controller.status == nil)
    }

    private func boolBinding(_ key: String, value: @escaping (ControllerStatus) -> Bool) -> Binding<Bool> {
        Binding(
            get: { controller.status.map(value) ?? false },
            set: { newValue in Task { await controller.setConfig([key: newValue]) } }
        )
    }

    private func intBinding(_ key: String, value: @escaping (ControllerStatus) -> Int) -> Binding<Int> {
        Binding(
            get: { controller.status.map(value) ?? 0 },
            set: { newValue in Task { await controller.setConfig([key: newValue]) } }
        )
    }
}

private struct NetworkView: View {
    @EnvironmentObject private var controller: BLEController
    @State private var apSSID = ""
    @State private var newAPPassword = ""
    @State private var selectedNetwork: ScannedUpstreamNetwork?

    var body: some View {
        Form {
            Section("ESP 热点") {
                TextField("热点名称", text: $apSSID)
                    .textInputAutocapitalization(.never)
                    .autocorrectionDisabled()
                SecureField("新密码（留空则不修改）", text: $newAPPassword)
                Button("保存热点设置") {
                    var values: [String: Any] = ["apSSID": apSSID]
                    if !newAPPassword.isEmpty { values["apPass"] = newAPPassword }
                    Task {
                        await controller.setConfig(values)
                        newAPPassword = ""
                    }
                }
                .disabled(apSSID.isEmpty || (!newAPPassword.isEmpty && !(8...63).contains(newAPPassword.count)))
            }
            Section("iPhone 上游热点") {
                Toggle("启用上游连接", isOn: Binding(
                    get: { controller.status?.upstreamEnable == 1 },
                    set: { value in Task { await controller.setConfig(["upstreamEnable": value]) } }
                ))
                LabeledContent("状态", value: controller.status?.upstreamStatus ?? "--")
                LabeledContent("当前热点", value: controller.status?.connectedUpstreamSSID.nonEmpty ?? "--")
                Button("扫描附近热点") { Task { await controller.scanUpstream() } }
            }
            if !controller.scannedNetworks.isEmpty {
                Section("扫描结果") {
                    ForEach(controller.scannedNetworks) { network in
                        Button {
                            selectedNetwork = network
                        } label: {
                            HStack {
                                VStack(alignment: .leading) {
                                    Text(network.ssid)
                                    Text("RSSI \(network.rssi) dBm")
                                        .font(.caption)
                                        .foregroundStyle(.secondary)
                                }
                                Spacer()
                                if network.saved { Image(systemName: "checkmark.circle.fill").foregroundStyle(.green) }
                                if network.secure { Image(systemName: "lock.fill").foregroundStyle(.secondary) }
                            }
                        }
                        .buttonStyle(.plain)
                    }
                }
            }
            Section("已保存热点") {
                if controller.status?.upstreamNetworks.isEmpty != false {
                    Text("暂无已保存热点").foregroundStyle(.secondary)
                }
                ForEach(controller.status?.upstreamNetworks ?? []) { network in
                    HStack {
                        VStack(alignment: .leading) {
                            Text(network.ssid)
                            if network.connected { Text("已连接").font(.caption).foregroundStyle(.green) }
                        }
                        Spacer()
                        Button(role: .destructive) {
                            Task { await controller.deleteUpstream(ssid: network.ssid) }
                        } label: {
                            Image(systemName: "trash")
                        }
                    }
                }
            }
        }
        .navigationTitle("网络")
        .disabled(!controller.connectionState.isConnected || controller.isBusy)
        .onAppear { apSSID = controller.status?.apSSID ?? "" }
        .onChange(of: controller.status?.apSSID) { _, value in apSSID = value ?? "" }
        .sheet(item: $selectedNetwork) { network in
            NetworkCredentialView(network: network)
                .environmentObject(controller)
        }
    }
}

private struct NetworkCredentialView: View {
    @EnvironmentObject private var controller: BLEController
    @Environment(\.dismiss) private var dismiss
    let network: ScannedUpstreamNetwork
    @State private var password = ""

    var body: some View {
        NavigationStack {
            Form {
                LabeledContent("热点", value: network.ssid)
                if network.secure { SecureField("热点密码", text: $password) }
                Text("密码只发送给 ESP 保存，不由 App 留存。")
                    .font(.footnote)
                    .foregroundStyle(.secondary)
            }
            .navigationTitle("保存上游热点")
            .toolbar {
                ToolbarItem(placement: .cancellationAction) { Button("取消") { dismiss() } }
                ToolbarItem(placement: .confirmationAction) {
                    Button("保存") {
                        Task {
                            await controller.saveUpstream(
                                ssid: network.ssid,
                                password: network.secure ? password : nil
                            )
                            password = ""
                            dismiss()
                        }
                    }
                    .disabled(network.secure && !(8...63).contains(password.count))
                }
            }
        }
    }
}

private struct DNSView: View {
    @EnvironmentObject private var controller: BLEController
    @State private var allowlist = ""
    @State private var blocklist = ""

    var body: some View {
        Form {
            Section("DNS 策略") {
                Toggle("启用严格白名单", isOn: Binding(
                    get: { controller.status?.dnsWhitelistEnable == 1 },
                    set: { value in Task { await controller.setConfig(["dnsWhitelistEnable": value]) } }
                ))
                LabeledContent("转发策略", value: controller.status?.dnsForwardPolicy ?? "--")
                LabeledContent("允许 IP", value: "\(controller.status?.dnsAllowIpCount ?? 0)")
                LabeledContent("拦截 IP", value: "\(controller.status?.dnsBlockIpCount ?? 0)")
            }
            Section("白名单") {
                TextEditor(text: $allowlist).frame(minHeight: 120)
            }
            Section("黑名单") {
                TextEditor(text: $blocklist).frame(minHeight: 100)
                Button("保存 DNS 规则") {
                    Task {
                        await controller.setConfig([
                            "dnsAllowlist": allowlist,
                            "dnsBlocklist": blocklist,
                        ])
                    }
                }
            }
            Section("最近拦截（\(controller.status?.dnsBlockedCount ?? 0) 次）") {
                if controller.status?.dnsBlockedRequests.isEmpty != false {
                    Text("暂无拦截记录").foregroundStyle(.secondary)
                }
                ForEach(controller.status?.dnsBlockedRequests ?? []) { item in
                    LabeledContent(item.domain, value: "\(item.count) 次 · \(relativeTime(item))")
                }
                Button("清空拦截记录", role: .destructive) {
                    Task { await controller.clearBlockedDNS() }
                }
            }
        }
        .navigationTitle("DNS")
        .disabled(!controller.connectionState.isConnected || controller.isBusy)
        .onAppear { syncRules() }
        .onChange(of: controller.status?.dnsAllowlist) { _, value in allowlist = value ?? "" }
        .onChange(of: controller.status?.dnsBlocklist) { _, value in blocklist = value ?? "" }
    }

    private func syncRules() {
        allowlist = controller.status?.dnsAllowlist ?? ""
        blocklist = controller.status?.dnsBlocklist ?? ""
    }

    private func relativeTime(_ item: BlockedDNSRequest) -> String {
        let delta = max(0, Int(controller.status?.uptime ?? 0) - Int(item.lastBlockedAt))
        if delta < 60 { return "\(delta) 秒前" }
        if delta < 3600 { return "\(delta / 60) 分前" }
        return "\(delta / 3600) 小时前"
    }
}

private struct DiagnosticsView: View {
    @EnvironmentObject private var controller: BLEController

    var body: some View {
        List {
            Section("CAN 计数") {
                LabeledContent("收包", value: "\(controller.status?.rx ?? 0)")
                LabeledContent("改包", value: "\(controller.status?.modified ?? 0)")
                LabeledContent("错误", value: "\(controller.status?.errors ?? 0)")
            }
            Section("设备") {
                LabeledContent("运行时间", value: formatUptime(controller.status?.uptime ?? 0))
                LabeledContent("复位原因", value: controller.status?.resetReason ?? "--")
                LabeledContent("可用堆内存", value: formatBytes(controller.status?.freeHeap ?? 0))
                LabeledContent("最低堆内存", value: formatBytes(controller.status?.minFreeHeap ?? 0))
                LabeledContent("温控", value: controller.status?.thermalStatus ?? "--")
            }
            Section("设备日志") {
                HStack {
                    Button("读取") { Task { await controller.readDebugLog() } }
                    Spacer()
                    Button("清空", role: .destructive) { Task { await controller.clearDebugLog() } }
                }
                if controller.diagnosticLog.isEmpty {
                    Text("尚未读取日志").foregroundStyle(.secondary)
                } else {
                    ScrollView(.horizontal) {
                        Text(controller.diagnosticLog)
                            .font(.system(.caption, design: .monospaced))
                            .textSelection(.enabled)
                    }
                }
            }
            Section("固件升级") {
                Text("首版 App 不传输固件。请连接 ESP 热点后，继续使用现有网页升级。")
                Link("打开 9.9.9.9", destination: URL(string: "http://9.9.9.9")!)
            }
            Section("设备管理") {
                Button("忘记并重新选择控制器", role: .destructive) {
                    controller.forgetController()
                }
            }
        }
        .navigationTitle("诊断")
        .refreshable { await controller.refreshStatus() }
    }

    private func formatUptime(_ seconds: UInt64) -> String {
        let hours = seconds / 3600
        let minutes = seconds % 3600 / 60
        return hours > 0 ? "\(hours) 小时 \(minutes) 分" : "\(minutes) 分 \(seconds % 60) 秒"
    }

    private func formatBytes(_ bytes: UInt64) -> String {
        ByteCountFormatter.string(fromByteCount: Int64(bytes), countStyle: .memory)
    }
}

private struct PairingView: View {
    @EnvironmentObject private var controller: BLEController
    let canCancel: Bool
    let onCancel: () -> Void
    @State private var selected: DiscoveredController?
    @State private var accessPointPassword = ""

    var body: some View {
        NavigationStack {
            List {
                Section {
                    Text(controller.connectionState.title)
                    Text("选择控制器，输入当前 ESP 热点密码。App 只临时计算六位配对码，不保存密码。")
                        .font(.footnote)
                        .foregroundStyle(.secondary)
                }
                Section("附近控制器") {
                    if controller.discoveredControllers.isEmpty {
                        HStack { ProgressView(); Text("正在扫描…") }
                    }
                    ForEach(controller.discoveredControllers) { item in
                        Button {
                            selected = item
                            accessPointPassword = ""
                        } label: {
                            HStack {
                                VStack(alignment: .leading) {
                                    Text(item.name)
                                    Text(item.id.uuidString)
                                        .font(.caption2)
                                        .foregroundStyle(.secondary)
                                }
                                Spacer()
                                Text("\(item.rssi) dBm").font(.caption)
                            }
                        }
                    }
                    Button("重新扫描") { controller.startScanning() }
                }
                if let selected {
                    Section("安全配对") {
                        LabeledContent("设备", value: selected.name)
                        SecureField("当前 ESP 热点密码", text: $accessPointPassword)
                            .textContentType(.password)
                        if accessPointPassword.count >= 8 {
                            VStack(alignment: .leading, spacing: 6) {
                                Text("系统配对框请输入")
                                    .font(.caption)
                                    .foregroundStyle(.secondary)
                                Text(BLEProtocolV1.pairingCode(for: accessPointPassword))
                                    .font(.system(size: 34, weight: .bold, design: .monospaced))
                                    .textSelection(.enabled)
                            }
                        }
                        Button("连接并配对") { controller.select(selected) }
                            .disabled(!(8...63).contains(accessPointPassword.count))
                    }
                }
            }
            .navigationTitle("连接 ESP")
            .toolbar {
                if canCancel {
                    ToolbarItem(placement: .cancellationAction) {
                        Button("取消", action: onCancel)
                    }
                }
            }
            .onAppear { controller.startScanning() }
            .onDisappear { accessPointPassword = "" }
        }
    }
}

private extension String {
    var nonEmpty: String? { isEmpty ? nil : self }
}
