import Foundation

struct SavedUpstreamNetwork: Codable, Equatable, Identifiable {
    let ssid: String
    let hasPass: Bool
    let connected: Bool
    let active: Bool
    var id: String { ssid }
}

struct ScannedUpstreamNetwork: Codable, Equatable, Identifiable {
    let ssid: String
    let rssi: Int
    let secure: Bool
    let saved: Bool
    var id: String { ssid }
}

struct BlockedDNSRequest: Codable, Equatable, Identifiable {
    let domain: String
    let count: UInt64
    let lastBlockedAt: UInt64
    var id: String { domain }
}

struct ControllerStatus: Decodable, Equatable {
    var rx: UInt64 = 0
    var modified: UInt64 = 0
    var errors: UInt64 = 0
    var uptime: UInt64 = 0
    var rxActive = false
    var modifiedActive = false
    var canReady = false
    var canOK = false
    var fsdTriggered = false
    var fsdEnable = 0
    var hwMode = 0
    var speedProfile = 0
    var profileMode = 0
    var detectedSpeedLimitKph = 0
    var appliedSpeedOffsetKph = 0
    var vehicleSpeedKph = 0.0
    var vehicleSpeedValid = false
    var chipTempC: Double?
    var thermalStatus = ""
    var thermalProtect = false
    var isaChime = 0
    var emergencyDet = 0
    var chinaMode = 0
    var apRunning = false
    var apSSID = ""
    var apIP = ""
    var apClients = 0
    var upstreamEnable = 0
    var upstreamConfigured = 0
    var upstreamConnected = false
    var upstreamSSID = ""
    var connectedUpstreamSSID = ""
    var upstreamRSSI: Int?
    var upstreamStatus = ""
    var upstreamSignal = ""
    var upstreamIP = ""
    var upstreamPhase = ""
    var upstreamRetryCount: UInt64 = 0
    var lastStaDisconnectReason = 0
    var upstreamNetworks: [SavedUpstreamNetwork] = []
    var dnsWhitelistEnable = 0
    var dnsAllowlist = ""
    var dnsBlocklist = ""
    var dnsBlockedCount: UInt64 = 0
    var dnsBlockedRequests: [BlockedDNSRequest] = []
    var dnsForwardPolicy = ""
    var dnsAllowIpCount: UInt64 = 0
    var dnsBlockIpCount: UInt64 = 0
    var natEnabled = 0
    var natStatus = ""
    var debugLogReady = false
    var debugLogBytes: UInt64 = 0
    var resetReason = ""
    var freeHeap: UInt64 = 0
    var minFreeHeap: UInt64 = 0

    enum CodingKeys: String, CodingKey {
        case rx, modified, errors, uptime, rxActive, modifiedActive, canReady, canOK
        case fsdTriggered, fsdEnable, hwMode, speedProfile, profileMode
        case detectedSpeedLimitKph, appliedSpeedOffsetKph, vehicleSpeedKph, vehicleSpeedValid
        case chipTempC, thermalStatus, thermalProtect, isaChime, emergencyDet, chinaMode
        case apRunning, apSSID, apIP, apClients
        case upstreamEnable, upstreamConfigured, upstreamConnected, upstreamSSID
        case connectedUpstreamSSID, upstreamRSSI, upstreamStatus, upstreamSignal, upstreamIP
        case upstreamPhase, upstreamRetryCount, lastStaDisconnectReason, upstreamNetworks
        case dnsWhitelistEnable, dnsAllowlist, dnsBlocklist, dnsBlockedCount, dnsBlockedRequests
        case dnsForwardPolicy, dnsAllowIpCount, dnsBlockIpCount, natEnabled, natStatus
        case debugLogReady, debugLogBytes, resetReason, freeHeap, minFreeHeap
    }

    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        rx = try c.decodeIfPresent(UInt64.self, forKey: .rx) ?? 0
        modified = try c.decodeIfPresent(UInt64.self, forKey: .modified) ?? 0
        errors = try c.decodeIfPresent(UInt64.self, forKey: .errors) ?? 0
        uptime = try c.decodeIfPresent(UInt64.self, forKey: .uptime) ?? 0
        rxActive = try c.decodeIfPresent(Bool.self, forKey: .rxActive) ?? false
        modifiedActive = try c.decodeIfPresent(Bool.self, forKey: .modifiedActive) ?? false
        canReady = try c.decodeIfPresent(Bool.self, forKey: .canReady) ?? false
        canOK = try c.decodeIfPresent(Bool.self, forKey: .canOK) ?? false
        fsdTriggered = try c.decodeIfPresent(Bool.self, forKey: .fsdTriggered) ?? false
        fsdEnable = try c.decodeIfPresent(Int.self, forKey: .fsdEnable) ?? 0
        hwMode = try c.decodeIfPresent(Int.self, forKey: .hwMode) ?? 0
        speedProfile = try c.decodeIfPresent(Int.self, forKey: .speedProfile) ?? 0
        profileMode = try c.decodeIfPresent(Int.self, forKey: .profileMode) ?? 0
        detectedSpeedLimitKph = try c.decodeIfPresent(Int.self, forKey: .detectedSpeedLimitKph) ?? 0
        appliedSpeedOffsetKph = try c.decodeIfPresent(Int.self, forKey: .appliedSpeedOffsetKph) ?? 0
        vehicleSpeedKph = try c.decodeIfPresent(Double.self, forKey: .vehicleSpeedKph) ?? 0
        vehicleSpeedValid = try c.decodeIfPresent(Bool.self, forKey: .vehicleSpeedValid) ?? false
        chipTempC = try c.decodeIfPresent(Double.self, forKey: .chipTempC)
        thermalStatus = try c.decodeIfPresent(String.self, forKey: .thermalStatus) ?? ""
        thermalProtect = try c.decodeIfPresent(Bool.self, forKey: .thermalProtect) ?? false
        isaChime = try c.decodeIfPresent(Int.self, forKey: .isaChime) ?? 0
        emergencyDet = try c.decodeIfPresent(Int.self, forKey: .emergencyDet) ?? 0
        chinaMode = try c.decodeIfPresent(Int.self, forKey: .chinaMode) ?? 0
        apRunning = try c.decodeIfPresent(Bool.self, forKey: .apRunning) ?? false
        apSSID = try c.decodeIfPresent(String.self, forKey: .apSSID) ?? ""
        apIP = try c.decodeIfPresent(String.self, forKey: .apIP) ?? ""
        apClients = try c.decodeIfPresent(Int.self, forKey: .apClients) ?? 0
        upstreamEnable = try c.decodeIfPresent(Int.self, forKey: .upstreamEnable) ?? 0
        upstreamConfigured = try c.decodeIfPresent(Int.self, forKey: .upstreamConfigured) ?? 0
        upstreamConnected = try c.decodeIfPresent(Bool.self, forKey: .upstreamConnected) ?? false
        upstreamSSID = try c.decodeIfPresent(String.self, forKey: .upstreamSSID) ?? ""
        connectedUpstreamSSID = try c.decodeIfPresent(String.self, forKey: .connectedUpstreamSSID) ?? ""
        upstreamRSSI = try c.decodeIfPresent(Int.self, forKey: .upstreamRSSI)
        upstreamStatus = try c.decodeIfPresent(String.self, forKey: .upstreamStatus) ?? ""
        upstreamSignal = try c.decodeIfPresent(String.self, forKey: .upstreamSignal) ?? ""
        upstreamIP = try c.decodeIfPresent(String.self, forKey: .upstreamIP) ?? ""
        upstreamPhase = try c.decodeIfPresent(String.self, forKey: .upstreamPhase) ?? ""
        upstreamRetryCount = try c.decodeIfPresent(UInt64.self, forKey: .upstreamRetryCount) ?? 0
        lastStaDisconnectReason = try c.decodeIfPresent(Int.self, forKey: .lastStaDisconnectReason) ?? 0
        upstreamNetworks = try c.decodeIfPresent([SavedUpstreamNetwork].self, forKey: .upstreamNetworks) ?? []
        dnsWhitelistEnable = try c.decodeIfPresent(Int.self, forKey: .dnsWhitelistEnable) ?? 0
        dnsAllowlist = try c.decodeIfPresent(String.self, forKey: .dnsAllowlist) ?? ""
        dnsBlocklist = try c.decodeIfPresent(String.self, forKey: .dnsBlocklist) ?? ""
        dnsBlockedCount = try c.decodeIfPresent(UInt64.self, forKey: .dnsBlockedCount) ?? 0
        dnsBlockedRequests = try c.decodeIfPresent([BlockedDNSRequest].self, forKey: .dnsBlockedRequests) ?? []
        dnsForwardPolicy = try c.decodeIfPresent(String.self, forKey: .dnsForwardPolicy) ?? ""
        dnsAllowIpCount = try c.decodeIfPresent(UInt64.self, forKey: .dnsAllowIpCount) ?? 0
        dnsBlockIpCount = try c.decodeIfPresent(UInt64.self, forKey: .dnsBlockIpCount) ?? 0
        natEnabled = try c.decodeIfPresent(Int.self, forKey: .natEnabled) ?? 0
        natStatus = try c.decodeIfPresent(String.self, forKey: .natStatus) ?? ""
        debugLogReady = try c.decodeIfPresent(Bool.self, forKey: .debugLogReady) ?? false
        debugLogBytes = try c.decodeIfPresent(UInt64.self, forKey: .debugLogBytes) ?? 0
        resetReason = try c.decodeIfPresent(String.self, forKey: .resetReason) ?? ""
        freeHeap = try c.decodeIfPresent(UInt64.self, forKey: .freeHeap) ?? 0
        minFreeHeap = try c.decodeIfPresent(UInt64.self, forKey: .minFreeHeap) ?? 0
    }
}
