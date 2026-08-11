import Foundation

public struct AOOPathProbeReport: Codable, Equatable, Sendable {
    public var durationSeconds: Double
    public var sampleCount: Int
    public var channelCount: Int
    public var sampleRate: Int
    public var streamBlockSize: Int
    public var packetSize: Int
    public var blockDurationMilliseconds: Double
    public var audioPayloadBytesPerBlock: Int
    public var rawAudioPayloadMegabitsPerSecond: Double
    public var minimumDatagramsPerBlock: Int
    public var minimumDatagramsPerSecond: Double
    public var minimumRoundTripMilliseconds: Double?
    public var medianRoundTripMilliseconds: Double?
    public var p95RoundTripMilliseconds: Double?
    public var p99RoundTripMilliseconds: Double?
    public var maximumRoundTripMilliseconds: Double?
    public var p95RoundTripExcursionMilliseconds: Double?
    public var inferredMinimumOneWayMilliseconds: Double?
    public var inferredP95OneWayMilliseconds: Double?
    public var estimatedMinimumStableAOOJitterTargetMilliseconds: Double?
    public var medianPacketLoss: Double?
    public var maximumPacketLoss: Double?
    public var isEstimateUsable: Bool

    public init(
        durationSeconds: Double,
        senderStatus: AOOSenderStatus,
        roundTripSamplesMilliseconds: [Double],
        packetLossSamples: [Double]
    ) {
        let validRoundTrips = roundTripSamplesMilliseconds
            .filter { $0.isFinite && $0 >= 0 }
            .sorted()
        let validPacketLoss = packetLossSamples
            .filter { $0.isFinite && $0 >= 0 }
            .map { min($0, 1) }
            .sorted()
        let channelCount = max(0, senderStatus.channelCount)
        let sampleRate = max(0, senderStatus.sampleRate)
        let streamBlockSize = max(0, senderStatus.streamBlockSize)
        let packetSize = max(0, senderStatus.packetSize)
        let blockDuration = sampleRate > 0
            ? Double(streamBlockSize) * 1_000 / Double(sampleRate)
            : 0
        let bytesPerSample = senderStatus.format.bytesPerSample
        let payloadBytes = channelCount * streamBlockSize * bytesPerSample
        let datagramsPerBlock = packetSize > 0 && payloadBytes > 0
            ? max(1, (payloadBytes + packetSize - 1) / packetSize)
            : 0
        let blocksPerSecond = streamBlockSize > 0
            ? Double(sampleRate) / Double(streamBlockSize)
            : 0
        let minimumRTT = validRoundTrips.first
        let p95RTT = Self.percentile(0.95, in: validRoundTrips)
        let p99RTT = Self.percentile(0.99, in: validRoundTrips)

        self.durationSeconds = max(0, durationSeconds)
        self.sampleCount = validRoundTrips.count
        self.channelCount = channelCount
        self.sampleRate = sampleRate
        self.streamBlockSize = streamBlockSize
        self.packetSize = packetSize
        self.blockDurationMilliseconds = blockDuration
        self.audioPayloadBytesPerBlock = payloadBytes
        self.rawAudioPayloadMegabitsPerSecond = Double(channelCount * sampleRate)
            * Double(bytesPerSample * 8) / 1_000_000
        self.minimumDatagramsPerBlock = datagramsPerBlock
        self.minimumDatagramsPerSecond = Double(datagramsPerBlock) * blocksPerSecond
        self.minimumRoundTripMilliseconds = minimumRTT
        self.medianRoundTripMilliseconds = Self.percentile(0.5, in: validRoundTrips)
        self.p95RoundTripMilliseconds = p95RTT
        self.p99RoundTripMilliseconds = p99RTT
        self.maximumRoundTripMilliseconds = validRoundTrips.last
        self.p95RoundTripExcursionMilliseconds = Self.difference(p95RTT, minimumRTT)
        self.inferredMinimumOneWayMilliseconds = minimumRTT.map { $0 * 0.5 }
        self.inferredP95OneWayMilliseconds = p95RTT.map { $0 * 0.5 }
        self.estimatedMinimumStableAOOJitterTargetMilliseconds = p99RTT.map {
            // RTT/2 assumes a roughly symmetric route. Two source blocks cover
            // packetization and one scheduling interval; this is a starting
            // point for validation, not a guarantee against future Wi-Fi tails.
            $0 * 0.5 + 2 * blockDuration
        }
        self.medianPacketLoss = Self.percentile(0.5, in: validPacketLoss)
        self.maximumPacketLoss = validPacketLoss.last
        self.isEstimateUsable = validRoundTrips.count >= 20
            && sampleRate > 0
            && streamBlockSize > 0
            && senderStatus.peerResponsive
    }

    private static func difference(_ lhs: Double?, _ rhs: Double?) -> Double? {
        guard let lhs, let rhs else { return nil }
        return max(0, lhs - rhs)
    }

    private static func percentile(_ percentile: Double, in sortedValues: [Double]) -> Double? {
        guard !sortedValues.isEmpty else { return nil }
        guard sortedValues.count > 1 else { return sortedValues[0] }
        let position = min(max(percentile, 0), 1) * Double(sortedValues.count - 1)
        let lowerIndex = Int(position.rounded(.down))
        let upperIndex = Int(position.rounded(.up))
        guard lowerIndex != upperIndex else { return sortedValues[lowerIndex] }
        let fraction = position - Double(lowerIndex)
        return sortedValues[lowerIndex]
            + (sortedValues[upperIndex] - sortedValues[lowerIndex]) * fraction
    }
}
