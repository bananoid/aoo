import AOOAppleBridge
import Foundation
#if canImport(Darwin)
import Darwin
#endif

public enum AOOTransportError: Error, Equatable, LocalizedError, Sendable {
    case initializationFailed(code: Int32, message: String)
    case operationFailed(code: Int32, message: String)
    case invalidConfiguration(String)
    case portInUse(Int)

    public var errorDescription: String? {
        switch self {
        case let .initializationFailed(_, message), let .operationFailed(_, message):
            return message
        case let .invalidConfiguration(message):
            return message
        case let .portInUse(port):
            return "UDP port \(port) is already in use. Select another receiver port and use the same destination port in the sender."
        }
    }
}

public struct AOOPeerConfiguration: Codable, Equatable, Sendable {
    public static let defaultReceiverPort = 9_999
    /// Zero asks the OS for an ephemeral UDP port, avoiding collisions between
    /// simulator, previews, and tests. AOO replies to the observed source port.
    public static let defaultSenderPort = 0
    public static let defaultSourceID: Int32 = 1
    public static let defaultSinkID: Int32 = 1

    public var isEnabled: Bool
    public var host: String
    public var receiverPort: Int

    public init(
        isEnabled: Bool = false,
        host: String = "",
        receiverPort: Int = AOOPeerConfiguration.defaultReceiverPort
    ) {
        self.isEnabled = isEnabled
        self.host = host.trimmingCharacters(in: .whitespacesAndNewlines)
        self.receiverPort = Self.clampedPort(receiverPort)
    }

    public var hasUsableEndpoint: Bool {
        !host.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty
            && (1...Int(UInt16.max)).contains(receiverPort)
    }

    public static func clampedPort(_ port: Int) -> Int {
        min(max(port, 1), Int(UInt16.max))
    }
}

public struct AOOSenderStatus: Codable, Equatable, Sendable {
    public var isReady: Bool
    public var isEnabled: Bool
    public var hasDestination: Bool
    public var peerResponsive: Bool
    public var localPort: Int
    public var channelCount: Int
    public var sampleRate: Int
    public var streamBlockSize: Int
    public var packetSize: Int
    public var profile: AOOTransportProfile
    public var format: AOOAudioFormat
    public var lastErrorCode: Int32
    public var lastErrorMessage: String?
    public var processCallCount: UInt64
    public var processedFrameCount: UInt64
    public var processErrorCount: UInt64
    public var handoffDropCount: UInt64
    public var resentFrameCount: UInt64
    public var pingEventCount: UInt64
    public var handoffLatencyMilliseconds: Double
    public var maximumHandoffLatencyMilliseconds: Double
    public var sourcePresentationLeadMilliseconds: Double
    public var roundTripMilliseconds: Double
    public var packetLoss: Double
    public var realSampleRate: Double

    public init(
        isReady: Bool = false,
        isEnabled: Bool = false,
        hasDestination: Bool = false,
        peerResponsive: Bool = false,
        localPort: Int = 0,
        channelCount: Int = 0,
        sampleRate: Int = 0,
        streamBlockSize: Int = 0,
        packetSize: Int = 0,
        profile: AOOTransportProfile = .deterministicWired,
        format: AOOAudioFormat = .float32,
        lastErrorCode: Int32 = 0,
        lastErrorMessage: String? = nil,
        processCallCount: UInt64 = 0,
        processedFrameCount: UInt64 = 0,
        processErrorCount: UInt64 = 0,
        handoffDropCount: UInt64 = 0,
        resentFrameCount: UInt64 = 0,
        pingEventCount: UInt64 = 0,
        handoffLatencyMilliseconds: Double = 0,
        maximumHandoffLatencyMilliseconds: Double = 0,
        sourcePresentationLeadMilliseconds: Double = 0,
        roundTripMilliseconds: Double = 0,
        packetLoss: Double = 0,
        realSampleRate: Double = 0
    ) {
        self.isReady = isReady
        self.isEnabled = isEnabled
        self.hasDestination = hasDestination
        self.peerResponsive = peerResponsive
        self.localPort = localPort
        self.channelCount = channelCount
        self.sampleRate = sampleRate
        self.streamBlockSize = streamBlockSize
        self.packetSize = packetSize
        self.profile = profile
        self.format = format
        self.lastErrorCode = lastErrorCode
        self.lastErrorMessage = lastErrorMessage
        self.processCallCount = processCallCount
        self.processedFrameCount = processedFrameCount
        self.processErrorCount = processErrorCount
        self.handoffDropCount = handoffDropCount
        self.resentFrameCount = resentFrameCount
        self.pingEventCount = pingEventCount
        self.handoffLatencyMilliseconds = handoffLatencyMilliseconds
        self.maximumHandoffLatencyMilliseconds = maximumHandoffLatencyMilliseconds
        self.sourcePresentationLeadMilliseconds = sourcePresentationLeadMilliseconds
        self.roundTripMilliseconds = roundTripMilliseconds
        self.packetLoss = packetLoss
        self.realSampleRate = realSampleRate
    }
}

public final class AOOSender: @unchecked Sendable {
    private let handle: OpaquePointer
    private var nextSourceSamplePosition: UInt64 = 0
    public let configuration: AOOStreamConfiguration
    public let channelCount: Int
    public let sampleRate: Double
    public let maximumBlockSize: Int
    public let sourceChannelIndices: [Int]
    public let requiredSourceChannelCount: Int

    public init(
        localPort: Int = AOOPeerConfiguration.defaultSenderPort,
        sourceID: Int32 = AOOPeerConfiguration.defaultSourceID,
        configuration: AOOStreamConfiguration
    ) throws {
        let configuration = try configuration.validated()
        let resolvedSourceChannelIndices = configuration.channelMap
        let bridgeSourceChannelIndices = resolvedSourceChannelIndices.map(Int32.init)
        var errorCode: Int32 = 0
        let handle = bridgeSourceChannelIndices.withUnsafeBufferPointer { sourceChannels in
            AOOAppleSenderCreate(
                Int32(localPort == 0 ? 0 : AOOPeerConfiguration.clampedPort(localPort)),
                sourceID,
                Int32(configuration.channelCount),
                sourceChannels.baseAddress,
                Int32(sourceChannels.count),
                configuration.sampleRate,
                Int32(configuration.maximumCallbackFrames),
                Int32(configuration.blockSize),
                Int32(configuration.datagramSize),
                configuration.targetLatencyMilliseconds,
                configuration.profile.rawValue,
                configuration.format.rawValue,
                configuration.capabilities.rawValue,
                &errorCode
            )
        }
        guard let handle else {
            throw AOOTransportError.initializationFailed(
                code: errorCode,
                message: Self.errorMessage(code: errorCode, prefix: "AOO sender initialization failed")
            )
        }
        self.handle = handle
        self.configuration = configuration
        self.channelCount = configuration.channelCount
        self.sampleRate = configuration.sampleRate
        self.maximumBlockSize = configuration.maximumCallbackFrames
        self.sourceChannelIndices = resolvedSourceChannelIndices
        self.requiredSourceChannelCount = (resolvedSourceChannelIndices.max() ?? 0) + 1
    }

    deinit {
        AOOAppleSenderDestroy(handle)
    }

    public func apply(_ configuration: AOOPeerConfiguration) throws {
        if configuration.hasUsableEndpoint {
            let result = configuration.host.withCString { host in
                AOOAppleSenderSetDestination(
                    handle,
                    host,
                    Int32(configuration.receiverPort),
                    AOOPeerConfiguration.defaultSinkID
                )
            }
            try Self.check(result, operation: "AOO receiver configuration failed")
        } else {
            AOOAppleSenderClearDestination(handle)
        }
        try setEnabled(configuration.isEnabled && configuration.hasUsableEndpoint)
    }

    public func setEnabled(_ isEnabled: Bool) throws {
        try Self.check(
            AOOAppleSenderSetEnabled(handle, isEnabled ? 1 : 0),
            operation: isEnabled ? "AOO stream start failed" : "AOO stream stop failed"
        )
    }

    func setSimulatedPacketLoss(_ fraction: Float) throws {
        try Self.check(
            AOOAppleSenderSetSimulatedPacketLoss(handle, fraction),
            operation: "AOO packet-loss simulation failed"
        )
    }

    @inline(__always)
    public static func currentNTPTime() -> UInt64 {
        AOOAppleCurrentNTPTime()
    }

    @inline(__always)
    public static func timestamp(_ timestamp: UInt64, offsetByNanoseconds offset: Int64) -> UInt64 {
        AOOAppleNTPTimeOffsetNanoseconds(timestamp, offset)
    }

    @inline(__always)
    public func timestamp(_ timestamp: UInt64, offsetByFrames frameOffset: Int) -> UInt64 {
        AOOAppleNTPTimeOffsetFrames(
            timestamp,
            Int64(frameOffset),
            sampleRate
        )
    }

    @inline(__always)
    public func processPlanar(
        baseAddress: UnsafePointer<Float>,
        channelStride: Int,
        frameOffset: Int,
        frameCount: Int,
        sourceChannelCount: Int? = nil,
        timing: AOOAudioTiming? = nil
    ) {
        let processTime = timing?.sourceTimestamp ?? Self.currentNTPTime()
        let samplePosition = timing?.sourceSamplePosition ?? nextSourceSamplePosition
        _ = AOOAppleSenderProcessPlanarAtTime(
            handle,
            baseAddress,
            Int32(channelStride),
            Int32(frameOffset),
            Int32(sourceChannelCount ?? requiredSourceChannelCount),
            Int32(frameCount),
            samplePosition,
            processTime,
            timing?.sourcePresentationTimestamp ?? processTime
        )
        nextSourceSamplePosition = samplePosition &+ UInt64(frameCount)
    }

    @inline(__always)
    public func processPlanar(
        baseAddress: UnsafePointer<Float>,
        channelStride: Int,
        frameOffset: Int,
        frameCount: Int,
        sourceChannelCount: Int? = nil,
        sourceTimestamp: UInt64?,
        sourcePresentationTimestamp: UInt64?
    ) {
        let processTime = sourceTimestamp ?? Self.currentNTPTime()
        processPlanar(
            baseAddress: baseAddress,
            channelStride: channelStride,
            frameOffset: frameOffset,
            frameCount: frameCount,
            sourceChannelCount: sourceChannelCount,
            timing: AOOAudioTiming(
                sourceSamplePosition: nextSourceSamplePosition,
                sourceTimestamp: processTime,
                sourcePresentationTimestamp: sourcePresentationTimestamp ?? processTime
            )
        )
    }

    @inline(__always)
    public func processSilence(
        frameCount: Int,
        timing: AOOAudioTiming? = nil
    ) {
        let processTime = timing?.sourceTimestamp ?? Self.currentNTPTime()
        let samplePosition = timing?.sourceSamplePosition ?? nextSourceSamplePosition
        _ = AOOAppleSenderProcessSilenceAtTime(
            handle,
            Int32(frameCount),
            samplePosition,
            processTime,
            timing?.sourcePresentationTimestamp ?? processTime
        )
        nextSourceSamplePosition = samplePosition &+ UInt64(frameCount)
    }

    @inline(__always)
    public func processSilence(
        frameCount: Int,
        sourceTimestamp: UInt64?,
        sourcePresentationTimestamp: UInt64?
    ) {
        let processTime = sourceTimestamp ?? Self.currentNTPTime()
        processSilence(
            frameCount: frameCount,
            timing: AOOAudioTiming(
                sourceSamplePosition: nextSourceSamplePosition,
                sourceTimestamp: processTime,
                sourcePresentationTimestamp: sourcePresentationTimestamp ?? processTime
            )
        )
    }

    public var status: AOOSenderStatus {
        var raw = AOOAppleSenderStatus()
        AOOAppleSenderGetStatus(handle, &raw)
        return AOOSenderStatus(
            isReady: raw.isReady != 0,
            isEnabled: raw.isEnabled != 0,
            hasDestination: raw.hasDestination != 0,
            peerResponsive: raw.peerResponsive != 0,
            localPort: Int(raw.localPort),
            channelCount: Int(raw.channelCount),
            sampleRate: Int(raw.sampleRate),
            streamBlockSize: Int(raw.streamBlockSize),
            packetSize: Int(raw.packetSize),
            profile: AOOTransportProfile(rawValue: raw.transportProfile)
                ?? configuration.profile,
            format: AOOAudioFormat(rawValue: raw.pcmFormat)
                ?? configuration.format,
            lastErrorCode: raw.lastError,
            lastErrorMessage: raw.lastError == 0 ? nil : Self.errorMessage(code: raw.lastError),
            processCallCount: raw.processCallCount,
            processedFrameCount: raw.processedFrameCount,
            processErrorCount: raw.processErrorCount,
            handoffDropCount: raw.handoffDropCount,
            resentFrameCount: raw.resentFrameCount,
            pingEventCount: raw.pingEventCount,
            handoffLatencyMilliseconds: raw.handoffLatencyMilliseconds,
            maximumHandoffLatencyMilliseconds: raw.maximumHandoffLatencyMilliseconds,
            sourcePresentationLeadMilliseconds: raw.sourcePresentationLeadMilliseconds,
            roundTripMilliseconds: raw.roundTripMilliseconds,
            packetLoss: raw.packetLoss,
            realSampleRate: raw.realSampleRate
        )
    }

    private static func check(_ result: Int32, operation: String) throws {
        guard result == 0 else {
            throw AOOTransportError.operationFailed(
                code: result,
                message: errorMessage(code: result, prefix: operation)
            )
        }
    }

    private static func errorMessage(code: Int32, prefix: String? = nil) -> String {
        let detail = AOOAppleErrorString(code).map(String.init(cString:)) ?? "Unknown AOO error"
        guard let prefix else { return detail }
        return "\(prefix): \(detail)"
    }
}

public enum AOOReceiverStreamState: String, Codable, Equatable, Sendable {
    case inactive
    case active
    case buffering

    init(aooRawValue: Int32) {
        switch aooRawValue {
        case 1:
            self = .active
        case 2:
            self = .buffering
        default:
            self = .inactive
        }
    }
}

public struct AOOReceiverStatus: Codable, Equatable, Sendable {
    public var isReady: Bool
    public var streamActive: Bool
    public var streamState: AOOReceiverStreamState
    public var localPort: Int
    public var channelCount: Int
    public var sampleRate: Int
    public var maximumBlockSize: Int
    public var fixedBlockSizeEnabled: Bool
    public var dynamicResamplingEnabled: Bool
    public var lastProcessFrameCount: Int
    public var minimumProcessFrameCount: Int
    public var maximumProcessFrameCount: Int
    public var sourceChannelCount: Int
    public var sourceSampleRate: Int
    public var sourceBlockSize: Int
    public var profile: AOOTransportProfile
    public var format: AOOAudioFormat
    public var lastErrorCode: Int32
    public var lastErrorMessage: String?
    public var processCallCount: UInt64
    public var processedFrameCount: UInt64
    public var processErrorCount: UInt64
    public var processBlockMismatchCount: UInt64
    public var streamStartCount: UInt64
    public var streamActiveCount: UInt64
    public var streamBufferingCount: UInt64
    public var streamInactiveCount: UInt64
    public var bufferUnderrunCount: UInt64
    public var bufferOverrunCount: UInt64
    public var droppedBlockCount: UInt64
    public var resentBlockCount: UInt64
    public var sourceXRunCount: UInt64
    public var concealmentCount: UInt64
    public var reacquisitionCount: UInt64
    public var incompatibleStreamCount: UInt64
    public var adaptiveAdjustmentCount: UInt64
    public var arrivalObservationCount: UInt64
    public var sourceSamplePosition: UInt64
    public var timestampPingCount: UInt64
    public var aooStreamTimeSampleCount: UInt64
    public var streamTimeSampleCount: UInt64
    public var presentationTimestampSampleCount: UInt64
    public var sourceLatencyMilliseconds: Double
    public var sinkLatencyMilliseconds: Double
    public var jitterBufferLatencyMilliseconds: Double
    public var latestRoundTripMilliseconds: Double?
    public var latestRawClockOffsetMilliseconds: Double?
    public var latestRawAOOStreamTimeDeltaMilliseconds: Double?
    public var latestRawStreamTimeDeltaMilliseconds: Double?
    public var latestRawPresentationDeltaMilliseconds: Double?
    public var roundTripMilliseconds: Double?
    public var estimatedNetworkOneWayMilliseconds: Double?
    public var estimatedClockOffsetMilliseconds: Double?
    public var aooStreamToSinkCallbackLatencyMilliseconds: Double?
    public var sourceCallbackToSinkCallbackLatencyMilliseconds: Double?
    public var sourcePresentationToSinkCallbackLatencyMilliseconds: Double?
    public var outputPresentationLatencyMilliseconds: Double
    public var latestArrivalResidualMilliseconds: Double
    public var p99ArrivalResidualMilliseconds: Double
    public var estimatedPresentationToPresentationLatencyMilliseconds: Double?
    public var targetLatencyMilliseconds: Double
    public var bufferCapacityMilliseconds: Double
    public var bufferFillRatio: Double
    public var bufferedAudioMilliseconds: Double
    public var realSampleRate: Double

    public init(
        isReady: Bool = false,
        streamActive: Bool = false,
        streamState: AOOReceiverStreamState = .inactive,
        localPort: Int = 0,
        channelCount: Int = 0,
        sampleRate: Int = 0,
        maximumBlockSize: Int = 0,
        fixedBlockSizeEnabled: Bool = false,
        dynamicResamplingEnabled: Bool = false,
        lastProcessFrameCount: Int = 0,
        minimumProcessFrameCount: Int = 0,
        maximumProcessFrameCount: Int = 0,
        sourceChannelCount: Int = 0,
        sourceSampleRate: Int = 0,
        sourceBlockSize: Int = 0,
        profile: AOOTransportProfile = .deterministicWired,
        format: AOOAudioFormat = .float32,
        lastErrorCode: Int32 = 0,
        lastErrorMessage: String? = nil,
        processCallCount: UInt64 = 0,
        processedFrameCount: UInt64 = 0,
        processErrorCount: UInt64 = 0,
        processBlockMismatchCount: UInt64 = 0,
        streamStartCount: UInt64 = 0,
        streamActiveCount: UInt64 = 0,
        streamBufferingCount: UInt64 = 0,
        streamInactiveCount: UInt64 = 0,
        bufferUnderrunCount: UInt64 = 0,
        bufferOverrunCount: UInt64 = 0,
        droppedBlockCount: UInt64 = 0,
        resentBlockCount: UInt64 = 0,
        sourceXRunCount: UInt64 = 0,
        concealmentCount: UInt64 = 0,
        reacquisitionCount: UInt64 = 0,
        incompatibleStreamCount: UInt64 = 0,
        adaptiveAdjustmentCount: UInt64 = 0,
        arrivalObservationCount: UInt64 = 0,
        sourceSamplePosition: UInt64 = 0,
        timestampPingCount: UInt64 = 0,
        aooStreamTimeSampleCount: UInt64 = 0,
        streamTimeSampleCount: UInt64 = 0,
        presentationTimestampSampleCount: UInt64 = 0,
        sourceLatencyMilliseconds: Double = 0,
        sinkLatencyMilliseconds: Double = 0,
        jitterBufferLatencyMilliseconds: Double = 0,
        latestRoundTripMilliseconds: Double? = nil,
        latestRawClockOffsetMilliseconds: Double? = nil,
        latestRawAOOStreamTimeDeltaMilliseconds: Double? = nil,
        latestRawStreamTimeDeltaMilliseconds: Double? = nil,
        latestRawPresentationDeltaMilliseconds: Double? = nil,
        roundTripMilliseconds: Double? = nil,
        estimatedNetworkOneWayMilliseconds: Double? = nil,
        estimatedClockOffsetMilliseconds: Double? = nil,
        aooStreamToSinkCallbackLatencyMilliseconds: Double? = nil,
        sourceCallbackToSinkCallbackLatencyMilliseconds: Double? = nil,
        sourcePresentationToSinkCallbackLatencyMilliseconds: Double? = nil,
        outputPresentationLatencyMilliseconds: Double = 0,
        latestArrivalResidualMilliseconds: Double = 0,
        p99ArrivalResidualMilliseconds: Double = 0,
        estimatedPresentationToPresentationLatencyMilliseconds: Double? = nil,
        targetLatencyMilliseconds: Double = 0,
        bufferCapacityMilliseconds: Double = 0,
        bufferFillRatio: Double = -1,
        bufferedAudioMilliseconds: Double = -1,
        realSampleRate: Double = 0
    ) {
        self.isReady = isReady
        self.streamActive = streamActive
        self.streamState = streamState
        self.localPort = localPort
        self.channelCount = channelCount
        self.sampleRate = sampleRate
        self.maximumBlockSize = maximumBlockSize
        self.fixedBlockSizeEnabled = fixedBlockSizeEnabled
        self.dynamicResamplingEnabled = dynamicResamplingEnabled
        self.lastProcessFrameCount = lastProcessFrameCount
        self.minimumProcessFrameCount = minimumProcessFrameCount
        self.maximumProcessFrameCount = maximumProcessFrameCount
        self.sourceChannelCount = sourceChannelCount
        self.sourceSampleRate = sourceSampleRate
        self.sourceBlockSize = sourceBlockSize
        self.profile = profile
        self.format = format
        self.lastErrorCode = lastErrorCode
        self.lastErrorMessage = lastErrorMessage
        self.processCallCount = processCallCount
        self.processedFrameCount = processedFrameCount
        self.processErrorCount = processErrorCount
        self.processBlockMismatchCount = processBlockMismatchCount
        self.streamStartCount = streamStartCount
        self.streamActiveCount = streamActiveCount
        self.streamBufferingCount = streamBufferingCount
        self.streamInactiveCount = streamInactiveCount
        self.bufferUnderrunCount = bufferUnderrunCount
        self.bufferOverrunCount = bufferOverrunCount
        self.droppedBlockCount = droppedBlockCount
        self.resentBlockCount = resentBlockCount
        self.sourceXRunCount = sourceXRunCount
        self.concealmentCount = concealmentCount
        self.reacquisitionCount = reacquisitionCount
        self.incompatibleStreamCount = incompatibleStreamCount
        self.adaptiveAdjustmentCount = adaptiveAdjustmentCount
        self.arrivalObservationCount = arrivalObservationCount
        self.sourceSamplePosition = sourceSamplePosition
        self.timestampPingCount = timestampPingCount
        self.aooStreamTimeSampleCount = aooStreamTimeSampleCount
        self.streamTimeSampleCount = streamTimeSampleCount
        self.presentationTimestampSampleCount = presentationTimestampSampleCount
        self.sourceLatencyMilliseconds = sourceLatencyMilliseconds
        self.sinkLatencyMilliseconds = sinkLatencyMilliseconds
        self.jitterBufferLatencyMilliseconds = jitterBufferLatencyMilliseconds
        self.latestRoundTripMilliseconds = latestRoundTripMilliseconds
        self.latestRawClockOffsetMilliseconds = latestRawClockOffsetMilliseconds
        self.latestRawAOOStreamTimeDeltaMilliseconds = latestRawAOOStreamTimeDeltaMilliseconds
        self.latestRawStreamTimeDeltaMilliseconds = latestRawStreamTimeDeltaMilliseconds
        self.latestRawPresentationDeltaMilliseconds = latestRawPresentationDeltaMilliseconds
        self.roundTripMilliseconds = roundTripMilliseconds
        self.estimatedNetworkOneWayMilliseconds = estimatedNetworkOneWayMilliseconds
        self.estimatedClockOffsetMilliseconds = estimatedClockOffsetMilliseconds
        self.aooStreamToSinkCallbackLatencyMilliseconds = aooStreamToSinkCallbackLatencyMilliseconds
        self.sourceCallbackToSinkCallbackLatencyMilliseconds = sourceCallbackToSinkCallbackLatencyMilliseconds
        self.sourcePresentationToSinkCallbackLatencyMilliseconds = sourcePresentationToSinkCallbackLatencyMilliseconds
        self.outputPresentationLatencyMilliseconds = outputPresentationLatencyMilliseconds
        self.latestArrivalResidualMilliseconds = latestArrivalResidualMilliseconds
        self.p99ArrivalResidualMilliseconds = p99ArrivalResidualMilliseconds
        self.estimatedPresentationToPresentationLatencyMilliseconds = estimatedPresentationToPresentationLatencyMilliseconds
        self.targetLatencyMilliseconds = targetLatencyMilliseconds
        self.bufferCapacityMilliseconds = bufferCapacityMilliseconds
        self.bufferFillRatio = bufferFillRatio
        self.bufferedAudioMilliseconds = bufferedAudioMilliseconds
        self.realSampleRate = realSampleRate
    }

    public var isReceivingStream: Bool {
        if streamActive { return true }
        return streamState == .buffering
            && sourceChannelCount > 0
            && processCallCount > 0
            && bufferFillRatio.isFinite
            && bufferFillRatio >= 0
    }

    public var aooInternalLatencyMilliseconds: Double {
        sourceLatencyMilliseconds
            + jitterBufferLatencyMilliseconds
            + sinkLatencyMilliseconds
    }
}

public final class AOOReceiver: @unchecked Sendable {
    private let handle: OpaquePointer
    private let timestampEstimatorLock = NSLock()
    private var timestampEstimator = AOOAudioTimestampEstimator()
    public let configuration: AOOStreamConfiguration
    public let channelCount: Int
    public let sampleRate: Double

    public init(
        localPort: Int = AOOPeerConfiguration.defaultReceiverPort,
        sinkID: Int32 = AOOPeerConfiguration.defaultSinkID,
        configuration: AOOStreamConfiguration,
        outputChannelCount: Int? = nil,
        fixedCallbackSize: Bool = false
    ) throws {
        let configuration = try configuration.validatedForReceiver()
        let channelCount = outputChannelCount
            ?? max(configuration.channelCount, (configuration.channelMap.max() ?? 0) + 1)
        guard (1...64).contains(channelCount) else {
            throw AOOTransportError.invalidConfiguration("The receiver requires 1 to 64 channels.")
        }
        let resolvedPort = AOOPeerConfiguration.clampedPort(localPort)
        var errorCode: Int32 = 0
        let bridgeSourceChannelIndices = configuration.channelMap.map(Int32.init)
        let handle = bridgeSourceChannelIndices.withUnsafeBufferPointer { sourceChannels in
            AOOAppleReceiverCreate(
                Int32(resolvedPort),
                sinkID,
                Int32(channelCount),
                Int32(configuration.channelCount),
                sourceChannels.baseAddress,
                Int32(sourceChannels.count),
                configuration.sampleRate,
                Int32(configuration.maximumCallbackFrames),
                Int32(configuration.blockSize),
                fixedCallbackSize ? 1 : 0,
                configuration.targetLatencyMilliseconds,
                Int32(configuration.datagramSize),
                configuration.profile.rawValue,
                configuration.format.rawValue,
                configuration.capabilities.rawValue,
                &errorCode
            )
        }
        guard let handle else {
            #if canImport(Darwin)
            if AOOAppleLastSocketErrorCode() == EADDRINUSE {
                throw AOOTransportError.portInUse(resolvedPort)
            }
            #endif
            throw AOOTransportError.initializationFailed(
                code: errorCode,
                message: Self.errorMessage(code: errorCode, prefix: "AOO receiver initialization failed")
            )
        }
        self.handle = handle
        self.configuration = configuration
        self.channelCount = channelCount
        self.sampleRate = configuration.sampleRate
    }

    deinit {
        AOOAppleReceiverDestroy(handle)
    }

    public func setLatency(milliseconds: Double) throws {
        let result = AOOAppleReceiverSetLatency(handle, milliseconds)
        guard result == 0 else {
            throw AOOTransportError.operationFailed(
                code: result,
                message: Self.errorMessage(code: result, prefix: "AOO latency update failed")
            )
        }
    }

    public func setMonitorPair(startingAtOneBased channel: Int) {
        AOOAppleReceiverSetMonitorPair(handle, Int32(channel))
    }

    public func setOutputPresentationLatency(milliseconds: Double) {
        AOOAppleReceiverSetOutputPresentationLatency(handle, milliseconds)
    }

    @inline(__always)
    public static func currentNTPTime() -> UInt64 {
        AOOAppleCurrentNTPTime()
    }

    @inline(__always)
    public func processStereo(
        outputLeft: UnsafeMutablePointer<Float>,
        outputRight: UnsafeMutablePointer<Float>,
        frameCount: Int,
        sinkNtpTime: UInt64? = nil
    ) {
        _ = AOOAppleReceiverProcessStereoAtTime(
            handle,
            outputLeft,
            outputRight,
            Int32(frameCount),
            sinkNtpTime ?? Self.currentNTPTime()
        )
    }

    public var status: AOOReceiverStatus {
        var raw = AOOAppleReceiverStatus()
        AOOAppleReceiverGetStatus(handle, &raw)
        let streamState = AOOReceiverStreamState(aooRawValue: raw.streamState)
        timestampEstimatorLock.lock()
        let timestampMetrics = timestampEstimator.update(
            AOOAudioTimestampObservation(
                streamStartCount: raw.streamStartCount,
                sourceIsConnected: raw.sourceChannelCount > 0,
                pingEventCount: raw.pingEventCount,
                aooStreamTimeEventCount: raw.aooStreamTimeEventCount,
                streamTimeEventCount: raw.streamTimeEventCount,
                presentationTimestampEventCount: raw.presentationTimestampEventCount,
                rawRoundTripMilliseconds: raw.rawRoundTripMilliseconds,
                rawClockOffsetMilliseconds: raw.rawClockOffsetMilliseconds,
                rawAOOStreamTimeDeltaMilliseconds: raw.rawAOOStreamTimeDeltaMilliseconds,
                rawStreamTimeDeltaMilliseconds: raw.rawStreamTimeDeltaMilliseconds,
                rawPresentationToSinkCallbackDeltaMilliseconds: raw.rawPresentationToSinkCallbackDeltaMilliseconds,
                outputPresentationLatencyMilliseconds: raw.outputPresentationLatencyMilliseconds
            )
        )
        timestampEstimatorLock.unlock()
        return AOOReceiverStatus(
            isReady: raw.isReady != 0,
            streamActive: raw.streamActive != 0,
            streamState: streamState,
            localPort: Int(raw.localPort),
            channelCount: Int(raw.channelCount),
            sampleRate: Int(raw.sampleRate),
            maximumBlockSize: Int(raw.maximumBlockSize),
            fixedBlockSizeEnabled: raw.fixedBlockSizeEnabled != 0,
            dynamicResamplingEnabled: raw.dynamicResamplingEnabled != 0,
            lastProcessFrameCount: Int(raw.lastProcessFrameCount),
            minimumProcessFrameCount: Int(raw.minimumProcessFrameCount),
            maximumProcessFrameCount: Int(raw.maximumProcessFrameCount),
            sourceChannelCount: Int(raw.sourceChannelCount),
            sourceSampleRate: Int(raw.sourceSampleRate),
            sourceBlockSize: Int(raw.sourceBlockSize),
            profile: AOOTransportProfile(rawValue: raw.transportProfile)
                ?? configuration.profile,
            format: AOOAudioFormat(rawValue: raw.pcmFormat)
                ?? configuration.format,
            lastErrorCode: raw.lastError,
            lastErrorMessage: raw.lastError == 0 ? nil : Self.errorMessage(code: raw.lastError),
            processCallCount: raw.processCallCount,
            processedFrameCount: raw.processedFrameCount,
            processErrorCount: raw.processErrorCount,
            processBlockMismatchCount: raw.processBlockMismatchCount,
            streamStartCount: raw.streamStartCount,
            streamActiveCount: raw.streamActiveCount,
            streamBufferingCount: raw.streamBufferingCount,
            streamInactiveCount: raw.streamInactiveCount,
            bufferUnderrunCount: raw.bufferUnderrunCount,
            bufferOverrunCount: raw.bufferOverrunCount,
            droppedBlockCount: raw.droppedBlockCount,
            resentBlockCount: raw.resentBlockCount,
            sourceXRunCount: raw.sourceXRunCount,
            concealmentCount: raw.concealmentCount,
            reacquisitionCount: raw.reacquisitionCount,
            incompatibleStreamCount: raw.incompatibleStreamCount,
            adaptiveAdjustmentCount: raw.adaptiveAdjustmentCount,
            arrivalObservationCount: raw.arrivalObservationCount,
            sourceSamplePosition: raw.sourceSamplePosition,
            timestampPingCount: raw.pingEventCount,
            aooStreamTimeSampleCount: raw.aooStreamTimeEventCount,
            streamTimeSampleCount: raw.streamTimeEventCount,
            presentationTimestampSampleCount: raw.presentationTimestampEventCount,
            sourceLatencyMilliseconds: raw.sourceLatencyMilliseconds,
            sinkLatencyMilliseconds: raw.sinkLatencyMilliseconds,
            jitterBufferLatencyMilliseconds: raw.jitterBufferLatencyMilliseconds,
            latestRoundTripMilliseconds: timestampMetrics.latestRoundTripMilliseconds,
            latestRawClockOffsetMilliseconds: raw.pingEventCount > 0
                ? raw.rawClockOffsetMilliseconds
                : nil,
            latestRawAOOStreamTimeDeltaMilliseconds: raw.aooStreamTimeEventCount > 0
                ? raw.rawAOOStreamTimeDeltaMilliseconds
                : nil,
            latestRawStreamTimeDeltaMilliseconds: raw.streamTimeEventCount > 0
                ? raw.rawStreamTimeDeltaMilliseconds
                : nil,
            latestRawPresentationDeltaMilliseconds: raw.presentationTimestampEventCount > 0
                ? raw.rawPresentationToSinkCallbackDeltaMilliseconds
                : nil,
            roundTripMilliseconds: timestampMetrics.roundTripMilliseconds,
            estimatedNetworkOneWayMilliseconds: timestampMetrics.estimatedNetworkOneWayMilliseconds,
            estimatedClockOffsetMilliseconds: timestampMetrics.estimatedClockOffsetMilliseconds,
            aooStreamToSinkCallbackLatencyMilliseconds: timestampMetrics.aooStreamToSinkCallbackLatencyMilliseconds,
            sourceCallbackToSinkCallbackLatencyMilliseconds: timestampMetrics.sourceCallbackToSinkCallbackLatencyMilliseconds,
            sourcePresentationToSinkCallbackLatencyMilliseconds: timestampMetrics.sourcePresentationToSinkCallbackLatencyMilliseconds,
            outputPresentationLatencyMilliseconds: timestampMetrics.outputPresentationLatencyMilliseconds,
            latestArrivalResidualMilliseconds: raw.latestArrivalResidualMilliseconds,
            p99ArrivalResidualMilliseconds: raw.p99ArrivalResidualMilliseconds,
            estimatedPresentationToPresentationLatencyMilliseconds: timestampMetrics.estimatedPresentationToPresentationLatencyMilliseconds,
            targetLatencyMilliseconds: raw.targetLatencyMilliseconds,
            bufferCapacityMilliseconds: raw.bufferCapacityMilliseconds,
            bufferFillRatio: raw.bufferFillRatio,
            bufferedAudioMilliseconds: raw.bufferedAudioMilliseconds,
            realSampleRate: raw.realSampleRate
        )
    }

    public func channelPeaks() -> [Float] {
        var peaks = Array(repeating: Float.zero, count: channelCount)
        let copied = peaks.withUnsafeMutableBufferPointer { buffer in
            AOOAppleReceiverCopyChannelPeaks(
                handle,
                buffer.baseAddress,
                Int32(buffer.count)
            )
        }
        if copied < peaks.count {
            peaks.removeSubrange(Int(copied)..<peaks.count)
        }
        return peaks
    }

    private static func errorMessage(code: Int32, prefix: String? = nil) -> String {
        let detail = AOOAppleErrorString(code).map(String.init(cString:)) ?? "Unknown AOO error"
        guard let prefix else { return detail }
        return "\(prefix): \(detail)"
    }
}
