import Foundation

public extension AOOSender {
    var transportStatus: AOOTransportStatus {
        let status = status
        return AOOTransportStatus(
            isRunning: status.isEnabled,
            profile: status.profile,
            format: status.format,
            targetLatencyMilliseconds: configuration.targetLatencyMilliseconds,
            effectiveLatencyMilliseconds: configuration.targetLatencyMilliseconds,
            health: currentTransportHealth(for: status),
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
        return AOOTransportStatus(
            isRunning: status.isReceivingStream,
            profile: status.profile,
            format: status.format,
            targetLatencyMilliseconds: status.targetLatencyMilliseconds,
            effectiveLatencyMilliseconds: status.aooInternalLatencyMilliseconds,
            bufferFillRatio: status.bufferFillRatio,
            health: currentTransportHealth(for: status),
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

func currentTransportHealth(for status: AOOSenderStatus) -> AOOTransportHealth {
    if !status.isEnabled {
        return .stopped
    }
    if status.lastErrorCode != 0 {
        return .failed
    }
    return status.peerResponsive ? .stable : .acquiring
}

func currentTransportHealth(for status: AOOReceiverStatus) -> AOOTransportHealth {
    if status.lastErrorCode != 0 {
        return status.incompatibleStreamCount > 0 ? .incompatible : .failed
    }
    switch status.streamState {
    case .inactive:
        return status.sourceChannelCount > 0 ? .acquiring : .stopped
    case .buffering:
        return .acquiring
    case .active:
        return .stable
    }
}

private func driftPartsPerMillion(nominal: Double, measured: Double) -> Double {
    guard nominal.isFinite, nominal > 0, measured.isFinite, measured > 0 else {
        return 0
    }
    return (measured / nominal - 1) * 1_000_000
}
