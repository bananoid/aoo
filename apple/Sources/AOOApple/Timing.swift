import Foundation

struct AOOAudioTimestampObservation: Equatable, Sendable {
    var streamStartCount: UInt64
    var sourceIsConnected: Bool
    var pingEventCount: UInt64
    var aooStreamTimeEventCount: UInt64
    var streamTimeEventCount: UInt64
    var presentationTimestampEventCount: UInt64
    var rawRoundTripMilliseconds: Double
    var rawClockOffsetMilliseconds: Double
    var rawAOOStreamTimeDeltaMilliseconds: Double
    var rawStreamTimeDeltaMilliseconds: Double
    var rawPresentationToSinkCallbackDeltaMilliseconds: Double
    var outputPresentationLatencyMilliseconds: Double
}

struct AOOAudioTimestampMetrics: Equatable, Sendable {
    var latestRoundTripMilliseconds: Double?
    var roundTripMilliseconds: Double?
    var estimatedNetworkOneWayMilliseconds: Double?
    var estimatedClockOffsetMilliseconds: Double?
    var aooStreamToSinkCallbackLatencyMilliseconds: Double?
    var sourceCallbackToSinkCallbackLatencyMilliseconds: Double?
    var sourcePresentationToSinkCallbackLatencyMilliseconds: Double?
    var outputPresentationLatencyMilliseconds: Double
    var estimatedPresentationToPresentationLatencyMilliseconds: Double?
}

struct AOOAudioTimestampEstimator: Sendable {
    private struct PingSample: Equatable, Sendable {
        var roundTripMilliseconds: Double
        var clockOffsetMilliseconds: Double
    }

    private static let pingWindowCapacity = 8
    private static let maximumPlausibleLatencyMilliseconds = 5_000.0

    private var streamStartCount: UInt64?
    private var lastPingEventCount: UInt64 = 0
    private var lastAOOStreamTimeEventCount: UInt64 = 0
    private var lastStreamTimeEventCount: UInt64 = 0
    private var lastPresentationTimestampEventCount: UInt64 = 0
    private var pingSamples: [PingSample] = []
    private var latestAOOStreamTimeDeltaMilliseconds: Double?
    private var latestStreamTimeDeltaMilliseconds: Double?
    private var latestPresentationToSinkCallbackDeltaMilliseconds: Double?

    mutating func update(
        _ observation: AOOAudioTimestampObservation
    ) -> AOOAudioTimestampMetrics {
        if streamStartCount != observation.streamStartCount {
            let previousPingEventCount = lastPingEventCount
            let previousAOOStreamTimeEventCount = lastAOOStreamTimeEventCount
            let previousStreamTimeEventCount = lastStreamTimeEventCount
            let previousPresentationTimestampEventCount = lastPresentationTimestampEventCount
            streamStartCount = observation.streamStartCount
            lastPingEventCount = observation.pingEventCount
            lastAOOStreamTimeEventCount = observation.aooStreamTimeEventCount
            lastStreamTimeEventCount = observation.streamTimeEventCount
            lastPresentationTimestampEventCount = observation.presentationTimestampEventCount
            pingSamples.removeAll(keepingCapacity: true)
            latestAOOStreamTimeDeltaMilliseconds = nil
            latestStreamTimeDeltaMilliseconds = nil
            latestPresentationToSinkCallbackDeltaMilliseconds = nil
            if observation.pingEventCount != previousPingEventCount {
                appendPingSample(from: observation)
            }
            if observation.aooStreamTimeEventCount != previousAOOStreamTimeEventCount {
                captureAOOStreamTime(from: observation)
            }
            if observation.streamTimeEventCount != previousStreamTimeEventCount {
                captureStreamTime(from: observation)
            }
            if observation.presentationTimestampEventCount
                != previousPresentationTimestampEventCount {
                capturePresentationTime(from: observation)
            }
        } else {
            if observation.pingEventCount != lastPingEventCount {
                lastPingEventCount = observation.pingEventCount
                appendPingSample(from: observation)
            }
            if observation.aooStreamTimeEventCount != lastAOOStreamTimeEventCount {
                lastAOOStreamTimeEventCount = observation.aooStreamTimeEventCount
                captureAOOStreamTime(from: observation)
            }
            if observation.streamTimeEventCount != lastStreamTimeEventCount {
                lastStreamTimeEventCount = observation.streamTimeEventCount
                captureStreamTime(from: observation)
            }
            if observation.presentationTimestampEventCount
                != lastPresentationTimestampEventCount {
                lastPresentationTimestampEventCount = observation.presentationTimestampEventCount
                capturePresentationTime(from: observation)
            }
        }

        if !observation.sourceIsConnected {
            latestAOOStreamTimeDeltaMilliseconds = nil
            latestStreamTimeDeltaMilliseconds = nil
            latestPresentationToSinkCallbackDeltaMilliseconds = nil
        }

        let selectedPing = pingSamples.min {
            $0.roundTripMilliseconds < $1.roundTripMilliseconds
        }
        let aooStreamLatency = selectedPing.flatMap { ping in
            latestAOOStreamTimeDeltaMilliseconds.flatMap { delta in
                Self.plausibleLatency(delta + ping.clockOffsetMilliseconds)
            }
        }
        let sourceCallbackLatency = selectedPing.flatMap { ping in
            latestStreamTimeDeltaMilliseconds.flatMap { delta in
                // AOO defines this offset as source clock - sink clock. The
                // stream delta is sink timestamp - source timestamp.
                Self.plausibleLatency(delta + ping.clockOffsetMilliseconds)
            }
        }
        let sourcePresentationLatency = selectedPing.flatMap { ping in
            latestPresentationToSinkCallbackDeltaMilliseconds.flatMap { delta in
                Self.plausibleLatency(delta + ping.clockOffsetMilliseconds)
            }
        }
        let outputLatency = max(
            0,
            observation.outputPresentationLatencyMilliseconds.isFinite
                ? observation.outputPresentationLatencyMilliseconds
                : 0
        )

        return AOOAudioTimestampMetrics(
            latestRoundTripMilliseconds: pingSamples.last?.roundTripMilliseconds,
            roundTripMilliseconds: selectedPing?.roundTripMilliseconds,
            estimatedNetworkOneWayMilliseconds: selectedPing.map {
                $0.roundTripMilliseconds * 0.5
            },
            estimatedClockOffsetMilliseconds: selectedPing?.clockOffsetMilliseconds,
            aooStreamToSinkCallbackLatencyMilliseconds: aooStreamLatency,
            sourceCallbackToSinkCallbackLatencyMilliseconds: sourceCallbackLatency,
            sourcePresentationToSinkCallbackLatencyMilliseconds: sourcePresentationLatency,
            outputPresentationLatencyMilliseconds: outputLatency,
            estimatedPresentationToPresentationLatencyMilliseconds: sourcePresentationLatency.map {
                $0 + outputLatency
            }
        )
    }

    private mutating func captureAOOStreamTime(
        from observation: AOOAudioTimestampObservation
    ) {
        guard observation.aooStreamTimeEventCount > 0,
              observation.rawAOOStreamTimeDeltaMilliseconds.isFinite else { return }
        latestAOOStreamTimeDeltaMilliseconds =
            observation.rawAOOStreamTimeDeltaMilliseconds
    }

    private mutating func appendPingSample(
        from observation: AOOAudioTimestampObservation
    ) {
        guard observation.pingEventCount > 0,
              observation.rawRoundTripMilliseconds.isFinite,
              observation.rawRoundTripMilliseconds >= 0,
              observation.rawRoundTripMilliseconds <= Self.maximumPlausibleLatencyMilliseconds,
              observation.rawClockOffsetMilliseconds.isFinite else { return }
        pingSamples.append(PingSample(
            roundTripMilliseconds: observation.rawRoundTripMilliseconds,
            clockOffsetMilliseconds: observation.rawClockOffsetMilliseconds
        ))
        if pingSamples.count > Self.pingWindowCapacity {
            pingSamples.removeFirst(pingSamples.count - Self.pingWindowCapacity)
        }
    }

    private mutating func captureStreamTime(
        from observation: AOOAudioTimestampObservation
    ) {
        guard observation.streamTimeEventCount > 0,
              observation.rawStreamTimeDeltaMilliseconds.isFinite else { return }
        latestStreamTimeDeltaMilliseconds = observation.rawStreamTimeDeltaMilliseconds
    }

    private mutating func capturePresentationTime(
        from observation: AOOAudioTimestampObservation
    ) {
        guard observation.presentationTimestampEventCount > 0,
              observation.rawPresentationToSinkCallbackDeltaMilliseconds.isFinite else { return }
        latestPresentationToSinkCallbackDeltaMilliseconds =
            observation.rawPresentationToSinkCallbackDeltaMilliseconds
    }

    private static func plausibleLatency(_ milliseconds: Double) -> Double? {
        guard milliseconds.isFinite,
              milliseconds >= 0,
              milliseconds <= maximumPlausibleLatencyMilliseconds else { return nil }
        return milliseconds
    }
}
