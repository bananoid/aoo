import Foundation

public enum AOOTransportProfile: Int32, Codable, CaseIterable, Sendable {
    case automatic = 0
    case deterministicWired = 1
    case adaptiveWireless = 2

    public func resolved(for interface: AOOInterfaceClass) -> AOOTransportProfile {
        guard self == .automatic else { return self }
        switch interface {
        case .direct, .wired:
            return .deterministicWired
        case .wifi, .other:
            return .adaptiveWireless
        }
    }
}

public enum AOOInterfaceClass: String, Codable, Sendable {
    case direct
    case wired
    case wifi
    case other
}

public enum AOOAudioFormat: Int32, Codable, CaseIterable, Sendable {
    case float32 = 1
    case int24 = 2

    public var bytesPerSample: Int {
        switch self {
        case .float32: 4
        case .int24: 3
        }
    }
}

public struct AOOStreamCapabilities: OptionSet, Codable, Sendable {
    public let rawValue: UInt32

    public init(rawValue: UInt32) {
        self.rawValue = rawValue
    }

    public static let absoluteSamplePosition = AOOStreamCapabilities(rawValue: 1 << 0)
    public static let channelMap = AOOStreamCapabilities(rawValue: 1 << 1)
    public static let dynamicResampling = AOOStreamCapabilities(rawValue: 1 << 2)
    public static let deadlineResend = AOOStreamCapabilities(rawValue: 1 << 3)

    public static let required: AOOStreamCapabilities = [
        .absoluteSamplePosition,
        .channelMap,
        .dynamicResampling
    ]

    public static func defaults(
        for profile: AOOTransportProfile
    ) -> AOOStreamCapabilities {
        profile == .adaptiveWireless
            ? [.required, .deadlineResend]
            : .required
    }
}

public struct AOOStreamConfiguration: Codable, Equatable, Sendable {
    public static let protocolVersion = 1

    public var protocolVersion: Int
    public var profile: AOOTransportProfile
    public var format: AOOAudioFormat
    public var channelCount: Int
    public var sampleRate: Double
    public var blockSize: Int
    public var maximumCallbackFrames: Int
    public var datagramSize: Int
    public var targetLatencyMilliseconds: Double
    public var channelMap: [Int]
    public var capabilities: AOOStreamCapabilities

    public init(
        protocolVersion: Int = AOOStreamConfiguration.protocolVersion,
        profile: AOOTransportProfile,
        format: AOOAudioFormat,
        channelCount: Int,
        sampleRate: Double,
        blockSize: Int = 64,
        maximumCallbackFrames: Int = 4_096,
        datagramSize: Int,
        targetLatencyMilliseconds: Double,
        channelMap: [Int]? = nil,
        capabilities: AOOStreamCapabilities? = nil
    ) {
        self.protocolVersion = protocolVersion
        self.profile = profile
        self.format = format
        self.channelCount = channelCount
        self.sampleRate = sampleRate
        self.blockSize = blockSize
        self.maximumCallbackFrames = maximumCallbackFrames
        self.datagramSize = datagramSize
        self.targetLatencyMilliseconds = targetLatencyMilliseconds
        self.channelMap = channelMap ?? Array(0..<channelCount)
        self.capabilities = capabilities ?? .defaults(for: profile)
    }

    public static func deterministicWired(
        channelCount: Int,
        sampleRate: Double = 48_000,
        channelMap: [Int]? = nil
    ) -> AOOStreamConfiguration {
        AOOStreamConfiguration(
            profile: .deterministicWired,
            format: .float32,
            channelCount: channelCount,
            sampleRate: sampleRate,
            blockSize: 64,
            datagramSize: 1_400,
            targetLatencyMilliseconds: max(4, 3 * 64 / sampleRate * 1_000),
            channelMap: channelMap
        )
    }

    public static func adaptiveWireless(
        channelCount: Int,
        sampleRate: Double = 48_000,
        initialRoundTripP99Milliseconds: Double? = nil,
        channelMap: [Int]? = nil
    ) -> AOOStreamConfiguration {
        let blockMilliseconds = 64 / sampleRate * 1_000
        let initialLatency = initialRoundTripP99Milliseconds.flatMap { p99 in
            p99.isFinite && p99 >= 0 ? max(8, p99 * 0.5 + 2 * blockMilliseconds) : nil
        } ?? 50
        return AOOStreamConfiguration(
            profile: .adaptiveWireless,
            format: .int24,
            channelCount: channelCount,
            sampleRate: sampleRate,
            blockSize: 64,
            datagramSize: 1_200,
            targetLatencyMilliseconds: initialLatency,
            channelMap: channelMap
        ).roundedToWholeBlocks()
    }

    public static func automaticReceiver(
        channelCapacity: Int,
        sampleRate: Double = 48_000
    ) -> AOOStreamConfiguration {
        AOOStreamConfiguration(
            profile: .automatic,
            format: .float32,
            channelCount: channelCapacity,
            sampleRate: sampleRate,
            blockSize: 64,
            datagramSize: 1_400,
            targetLatencyMilliseconds: 50,
            capabilities: .required.union(.deadlineResend)
        ).roundedToWholeBlocks()
    }

    public func validated() throws -> AOOStreamConfiguration {
        guard protocolVersion == Self.protocolVersion else {
            throw AOOTransportError.invalidConfiguration(
                "Unsupported low-latency protocol version \(protocolVersion)."
            )
        }
        guard profile != .automatic else {
            throw AOOTransportError.invalidConfiguration(
                "Resolve the automatic profile against a network path before starting."
            )
        }
        guard (1...64).contains(channelCount),
              sampleRate.isFinite,
              (8_000...384_000).contains(sampleRate),
              (16...4_096).contains(blockSize),
              maximumCallbackFrames >= blockSize,
              (256...4_096).contains(datagramSize),
              targetLatencyMilliseconds.isFinite,
              (2...200).contains(targetLatencyMilliseconds),
              channelMap.count == channelCount,
              Set(channelMap).count == channelCount,
              channelMap.allSatisfy({ (0..<64).contains($0) }),
              capabilities.contains(.required) else {
            throw AOOTransportError.invalidConfiguration("Invalid AOO stream configuration.")
        }
        switch profile {
        case .deterministicWired where format != .float32:
            throw AOOTransportError.invalidConfiguration(
                "The deterministic wired profile requires Float32 PCM."
            )
        case .adaptiveWireless where format != .int24:
            throw AOOTransportError.invalidConfiguration(
                "The adaptive wireless profile requires packed 24-bit PCM."
            )
        case .adaptiveWireless where !capabilities.contains(.deadlineResend):
            throw AOOTransportError.invalidConfiguration(
                "The adaptive wireless profile requires deadline-aware resend support."
            )
        default:
            break
        }
        return roundedToWholeBlocks()
    }

    func validatedForReceiver() throws -> AOOStreamConfiguration {
        guard profile == .automatic else { return try validated() }
        guard protocolVersion == Self.protocolVersion,
              (1...64).contains(channelCount),
              sampleRate.isFinite,
              (8_000...384_000).contains(sampleRate),
              (16...4_096).contains(blockSize),
              maximumCallbackFrames >= blockSize,
              (256...4_096).contains(datagramSize),
              targetLatencyMilliseconds.isFinite,
              (2...200).contains(targetLatencyMilliseconds),
              capabilities.contains(.required),
              capabilities.contains(.deadlineResend) else {
            throw AOOTransportError.invalidConfiguration(
                "Invalid automatic AOO receiver configuration."
            )
        }
        return roundedToWholeBlocks()
    }

    public var blockDurationMilliseconds: Double {
        Double(blockSize) / sampleRate * 1_000
    }

    public func roundedToWholeBlocks() -> AOOStreamConfiguration {
        var copy = self
        let duration = blockDurationMilliseconds
        guard duration.isFinite, duration > 0 else { return copy }
        let blocks = max(1, Int(ceil(targetLatencyMilliseconds / duration - 1e-9)))
        copy.targetLatencyMilliseconds = min(200, Double(blocks) * duration)
        return copy
    }
}

public struct AOOAudioTiming: Codable, Equatable, Sendable {
    public var sourceSamplePosition: UInt64
    public var sourceTimestamp: UInt64
    public var sourcePresentationTimestamp: UInt64

    public init(
        sourceSamplePosition: UInt64,
        sourceTimestamp: UInt64,
        sourcePresentationTimestamp: UInt64
    ) {
        self.sourceSamplePosition = sourceSamplePosition
        self.sourceTimestamp = sourceTimestamp
        self.sourcePresentationTimestamp = sourcePresentationTimestamp
    }
}

public struct AOOTransportStatistics: Codable, Equatable, Sendable {
    public var processedFrames: UInt64
    public var handoffDrops: UInt64
    public var latePackets: UInt64
    public var concealments: UInt64
    public var resends: UInt64
    public var reacquisitions: UInt64
    public var underruns: UInt64
    public var overruns: UInt64
    public var clockDriftPartsPerMillion: Double

    public init(
        processedFrames: UInt64 = 0,
        handoffDrops: UInt64 = 0,
        latePackets: UInt64 = 0,
        concealments: UInt64 = 0,
        resends: UInt64 = 0,
        reacquisitions: UInt64 = 0,
        underruns: UInt64 = 0,
        overruns: UInt64 = 0,
        clockDriftPartsPerMillion: Double = 0
    ) {
        self.processedFrames = processedFrames
        self.handoffDrops = handoffDrops
        self.latePackets = latePackets
        self.concealments = concealments
        self.resends = resends
        self.reacquisitions = reacquisitions
        self.underruns = underruns
        self.overruns = overruns
        self.clockDriftPartsPerMillion = clockDriftPartsPerMillion
    }
}

public struct AOOTransportStatus: Codable, Equatable, Sendable {
    public var isRunning: Bool
    public var profile: AOOTransportProfile
    public var format: AOOAudioFormat
    public var streamID: UInt64
    public var targetLatencyMilliseconds: Double
    public var effectiveLatencyMilliseconds: Double
    public var bufferFillRatio: Double
    public var health: AOOTransportHealth
    public var statistics: AOOTransportStatistics

    public init(
        isRunning: Bool = false,
        profile: AOOTransportProfile,
        format: AOOAudioFormat,
        streamID: UInt64 = 0,
        targetLatencyMilliseconds: Double = 0,
        effectiveLatencyMilliseconds: Double = 0,
        bufferFillRatio: Double = 0,
        health: AOOTransportHealth = .stopped,
        statistics: AOOTransportStatistics = .init()
    ) {
        self.isRunning = isRunning
        self.profile = profile
        self.format = format
        self.streamID = streamID
        self.targetLatencyMilliseconds = targetLatencyMilliseconds
        self.effectiveLatencyMilliseconds = effectiveLatencyMilliseconds
        self.bufferFillRatio = bufferFillRatio
        self.health = health
        self.statistics = statistics
    }
}

public enum AOOTransportHealth: String, Codable, Sendable {
    case stopped
    case acquiring
    case stable
    case concealing
    case reacquiring
    case incompatible
    case failed
}
