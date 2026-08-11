import Foundation

public struct AOOAdaptiveLatencyDecision: Equatable, Sendable {
    public enum Reason: Equatable, Sendable {
        case unchanged
        case deficit
        case sustainedHeadroom
    }

    public var targetLatencyMilliseconds: Double
    public var changed: Bool
    public var reason: Reason

    public init(
        targetLatencyMilliseconds: Double,
        changed: Bool,
        reason: Reason
    ) {
        self.targetLatencyMilliseconds = targetLatencyMilliseconds
        self.changed = changed
        self.reason = reason
    }
}

/// Non-realtime controller for adaptive Wi-Fi playout depth. Packet-arrival
/// residuals are retained in a fixed window so the transport never grows an
/// unbounded telemetry history.
public struct AOOAdaptiveLatencyController: Sendable {
    public static let residualCapacity = 4_096
    public static let adjustmentIntervalSeconds = 30.0

    public private(set) var targetLatencyMilliseconds: Double
    public let blockDurationMilliseconds: Double

    private var residuals = [Double](repeating: 0, count: residualCapacity)
    private var residualWriteIndex = 0
    private var residualCount = 0
    private var observationStartTime: TimeInterval?
    private var lastInstabilityTime: TimeInterval?
    private var lastAdjustmentTime: TimeInterval?

    public init(
        sampleRate: Double,
        blockSize: Int,
        initialRoundTripP99Milliseconds: Double? = nil
    ) {
        let blockDuration = Double(blockSize) / sampleRate * 1_000
        blockDurationMilliseconds = blockDuration
        let inferred = initialRoundTripP99Milliseconds.flatMap { value in
            value.isFinite && value >= 0
                ? max(8, value * 0.5 + 2 * blockDuration)
                : nil
        } ?? 50
        targetLatencyMilliseconds = Self.roundUp(
            inferred,
            blockDurationMilliseconds: blockDuration
        )
    }

    public mutating func observe(
        arrivalResidualMilliseconds: Double,
        wasLate: Bool,
        concealed: Bool,
        underrun: Bool,
        now: TimeInterval
    ) -> AOOAdaptiveLatencyDecision {
        if observationStartTime == nil {
            observationStartTime = now
        }
        if arrivalResidualMilliseconds.isFinite {
            residuals[residualWriteIndex] = arrivalResidualMilliseconds
            residualWriteIndex = (residualWriteIndex + 1) % Self.residualCapacity
            residualCount = min(residualCount + 1, Self.residualCapacity)
        }

        let arrivalJitter = max(0, arrivalResidualMilliseconds)
        let requiredTarget = Self.roundUp(
            arrivalJitter + 2 * blockDurationMilliseconds,
            blockDurationMilliseconds: blockDurationMilliseconds
        )
        if wasLate || concealed || underrun || requiredTarget > targetLatencyMilliseconds {
            lastInstabilityTime = now
            if requiredTarget > targetLatencyMilliseconds {
                targetLatencyMilliseconds = min(200, requiredTarget)
                lastAdjustmentTime = now
                return AOOAdaptiveLatencyDecision(
                    targetLatencyMilliseconds: targetLatencyMilliseconds,
                    changed: true,
                    reason: .deficit
                )
            }
        }

        let stabilityEpoch = lastInstabilityTime ?? observationStartTime ?? now
        let adjustmentEpoch = lastAdjustmentTime ?? observationStartTime ?? now
        let sinceInstability = now - stabilityEpoch
        let sinceAdjustment = now - adjustmentEpoch
        guard residualCount > 0,
              sinceInstability >= Self.adjustmentIntervalSeconds,
              sinceAdjustment >= Self.adjustmentIntervalSeconds else {
            return unchangedDecision
        }

        let p99 = percentile(0.99)
        let headroom = targetLatencyMilliseconds - max(0, p99)
        guard headroom >= 3 * blockDurationMilliseconds else {
            return unchangedDecision
        }

        let next = max(
            8,
            targetLatencyMilliseconds - blockDurationMilliseconds
        )
        guard next < targetLatencyMilliseconds else { return unchangedDecision }
        targetLatencyMilliseconds = next
        lastAdjustmentTime = now
        return AOOAdaptiveLatencyDecision(
            targetLatencyMilliseconds: targetLatencyMilliseconds,
            changed: true,
            reason: .sustainedHeadroom
        )
    }

    public func shouldRequestResend(
        millisecondsUntilDeadline: Double,
        roundTripP95Milliseconds: Double
    ) -> Bool {
        millisecondsUntilDeadline.isFinite
            && roundTripP95Milliseconds.isFinite
            && millisecondsUntilDeadline
                > roundTripP95Milliseconds + 2 * blockDurationMilliseconds
    }

    private var unchangedDecision: AOOAdaptiveLatencyDecision {
        AOOAdaptiveLatencyDecision(
            targetLatencyMilliseconds: targetLatencyMilliseconds,
            changed: false,
            reason: .unchanged
        )
    }

    private func percentile(_ fraction: Double) -> Double {
        guard residualCount > 0 else { return 0 }
        var values = [Double]()
        values.reserveCapacity(residualCount)
        for index in 0..<residualCount {
            let storedIndex = residualCount == Self.residualCapacity
                ? (residualWriteIndex + index) % Self.residualCapacity
                : index
            values.append(residuals[storedIndex])
        }
        values.sort()
        let offset = min(max(fraction, 0), 1) * Double(values.count - 1)
        let lower = Int(offset.rounded(.down))
        let upper = Int(offset.rounded(.up))
        guard lower != upper else { return values[lower] }
        let blend = offset - Double(lower)
        return values[lower] + (values[upper] - values[lower]) * blend
    }

    private static func roundUp(
        _ milliseconds: Double,
        blockDurationMilliseconds: Double
    ) -> Double {
        let bounded = min(200, max(2, milliseconds))
        let blocks = max(
            1,
            Int(ceil(bounded / blockDurationMilliseconds - 1e-9))
        )
        return min(200, Double(blocks) * blockDurationMilliseconds)
    }
}
