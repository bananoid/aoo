import Foundation

public extension AOOSender {
    var transportStatus: AOOTransportStatus {
        let status = status
        let health: AOOTransportHealth
        if !status.isEnabled {
            health = .stopped
        } else if status.handoffDropCount > 0 || status.processErrorCount > 0 {
            health = .failed
        } else if status.peerResponsive {
            health = .stable
        } else {
            health = .acquiring
        }
        return AOOTransportStatus(
            isRunning: status.isEnabled,
            profile: status.profile,
            format: status.format,
            targetLatencyMilliseconds: configuration.targetLatencyMilliseconds,
            effectiveLatencyMilliseconds: configuration.targetLatencyMilliseconds,
            health: health,
            statistics: AOOTransportStatistics(
                processedFrames: status.processedFrameCount,
                handoffDrops: status.handoffDropCount,
                resends: status.resentFrameCount,
                clockDriftPartsPerMillion: driftPartsPerMillion(
                    nominal: configuration.sampleRate,
                    measured: status.realSampleRate
                )
            )
        )
    }
}

public extension AOOReceiver {
    var transportStatus: AOOTransportStatus {
        let status = status
        let health: AOOTransportHealth
        if status.incompatibleStreamCount > 0 && status.lastErrorCode != 0 {
            health = .failed
        } else {
            switch status.streamState {
        case .inactive:
            health = status.sourceChannelCount > 0 ? .acquiring : .stopped
        case .buffering:
            health = status.reacquisitionCount > 0 ? .reacquiring : .acquiring
        case .active:
            health = status.concealmentCount > 0 ? .concealing : .stable
            }
        }
        return AOOTransportStatus(
            isRunning: status.isReceivingStream,
            profile: status.profile,
            format: status.format,
            targetLatencyMilliseconds: status.targetLatencyMilliseconds,
            effectiveLatencyMilliseconds: status.aooInternalLatencyMilliseconds,
            bufferFillRatio: status.bufferFillRatio,
            health: health,
            statistics: AOOTransportStatistics(
                processedFrames: status.processedFrameCount,
                latePackets: status.droppedBlockCount,
                concealments: status.concealmentCount,
                resends: status.resentBlockCount,
                reacquisitions: status.reacquisitionCount,
                underruns: status.bufferUnderrunCount,
                overruns: status.bufferOverrunCount,
                clockDriftPartsPerMillion: driftPartsPerMillion(
                    nominal: configuration.sampleRate,
                    measured: status.realSampleRate
                )
            )
        )
    }
}

private func driftPartsPerMillion(nominal: Double, measured: Double) -> Double {
    guard nominal.isFinite, nominal > 0, measured.isFinite, measured > 0 else {
        return 0
    }
    return (measured / nominal - 1) * 1_000_000
}
