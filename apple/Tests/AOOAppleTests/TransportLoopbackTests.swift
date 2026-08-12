import Darwin
import Foundation
import Testing
@testable import AOOApple

@Suite(.serialized)
struct TransportLoopbackTests {
    @Test func idleFixedReceiverProducesCleanSilence() throws {
        let port = testPort(offset: 3)
        var configuration = AOOStreamConfiguration
            .automaticReceiver(channelCapacity: 2)
        configuration.maximumCallbackFrames = 64
        let receiver = try AOOReceiver(
            localPort: port,
            configuration: configuration,
            fixedCallbackSize: true
        )
        receiver.setMonitorPair(startingAtOneBased: 1)

        var outputLeft = Array(repeating: Float(1), count: 64)
        var outputRight = Array(repeating: Float(-1), count: 64)
        for _ in 0..<8 {
            outputLeft.withUnsafeMutableBufferPointer { left in
                outputRight.withUnsafeMutableBufferPointer { right in
                    receiver.processStereo(
                        outputLeft: left.baseAddress!,
                        outputRight: right.baseAddress!,
                        frameCount: 64
                    )
                }
            }
        }

        let status = receiver.status
        #expect(outputLeft.allSatisfy { $0 == 0 })
        #expect(outputRight.allSatisfy { $0 == 0 })
        #expect(status.processCallCount == 8)
        #expect(status.processErrorCount == 0)
        #expect(status.processBlockMismatchCount == 0)
        #expect(status.minimumProcessFrameCount == 64)
        #expect(status.maximumProcessFrameCount == 64)
        #expect(receiver.transportStatus.health == .stopped)
    }

    @Test func currentHealthIgnoresHistoricalIncidentCounters() {
        let senderStatus = AOOSenderStatus(
            isEnabled: true,
            peerResponsive: true,
            processErrorCount: 2,
            handoffDropCount: 3
        )
        #expect(currentTransportHealth(for: senderStatus) == .stable)

        let receiverStatus = AOOReceiverStatus(
            streamActive: true,
            streamState: .active,
            sourceChannelCount: 2,
            concealmentCount: 4,
            reacquisitionCount: 1
        )
        #expect(currentTransportHealth(for: receiverStatus) == .stable)

        let incompatibleStatus = AOOReceiverStatus(
            streamState: .inactive,
            sourceChannelCount: 2,
            lastErrorCode: 1,
            incompatibleStreamCount: 1
        )
        #expect(currentTransportHealth(for: incompatibleStatus) == .incompatible)
    }

    @Test func deterministicWiredRestoresSparseTwentyChannelMap() throws {
        let port = testPort(offset: 0)
        var receiverConfiguration = AOOStreamConfiguration
            .automaticReceiver(channelCapacity: 20)
        receiverConfiguration.maximumCallbackFrames = 64
        let receiver = try AOOReceiver(
            localPort: port,
            configuration: receiverConfiguration,
            outputChannelCount: 20,
            fixedCallbackSize: true
        )
        receiver.setMonitorPair(startingAtOneBased: 5)

        var senderConfiguration = AOOStreamConfiguration.deterministicWired(
            channelCount: 2,
            channelMap: [4, 5]
        )
        senderConfiguration.maximumCallbackFrames = 64
        let sender = try AOOSender(configuration: senderConfiguration)
        try sender.apply(AOOPeerConfiguration(
            isEnabled: true,
            host: "127.0.0.1",
            receiverPort: port
        ))

        let peak = pump(
            sender: sender,
            receiver: receiver,
            sourceChannelCount: 6,
            sourceLeftChannel: 4,
            sourceRightChannel: 5,
            frameCount: 64,
            iterations: 900,
            injectNonfiniteSamples: true
        )

        #expect(peak > 0.05)
        #expect(waitUntil { receiver.status.streamActive })
        #expect(receiver.status.sourceChannelCount == 2)
        #expect(receiver.status.profile == .deterministicWired)
        #expect(receiver.status.format == .float32)
        #expect(receiver.status.completionObservationCount > 0)
        #expect(sender.status.handoffDropCount == 0)
        #expect(sender.status.processErrorCount == 0)
        #expect(receiver.status.processErrorCount == 0)
        #expect(throws: AOOTransportError.self) {
            try receiver.setLatency(milliseconds: 8)
        }
    }

    @Test func adaptiveWirelessTransportsPackedInt24() throws {
        let port = testPort(offset: 1)
        var configuration = AOOStreamConfiguration.adaptiveWireless(
            channelCount: 2
        )
        configuration.maximumCallbackFrames = 64
        var receiverConfiguration = AOOStreamConfiguration
            .automaticReceiver(channelCapacity: 2)
        receiverConfiguration.maximumCallbackFrames = 64
        let receiver = try AOOReceiver(
            localPort: port,
            configuration: receiverConfiguration,
            fixedCallbackSize: true
        )
        receiver.setMonitorPair(startingAtOneBased: 1)
        let sender = try AOOSender(configuration: configuration)
        try sender.apply(AOOPeerConfiguration(
            isEnabled: true,
            host: "127.0.0.1",
            receiverPort: port
        ))

        let peak = pump(
            sender: sender,
            receiver: receiver,
            sourceChannelCount: 2,
            sourceLeftChannel: 0,
            sourceRightChannel: 1,
            frameCount: 64,
            iterations: 1_200
        )

        #expect(peak > 0.05)
        #expect(receiver.status.profile == .adaptiveWireless)
        #expect(receiver.status.format == .int24)
        #expect(sender.status.processErrorCount == 0)
        #expect(receiver.status.processErrorCount == 0)
    }

    @Test func incompatibleProfileIsExplicitlyRejected() throws {
        let port = testPort(offset: 2)
        var receiverConfiguration = AOOStreamConfiguration
            .deterministicWired(channelCount: 2)
        receiverConfiguration.maximumCallbackFrames = 64
        let receiver = try AOOReceiver(
            localPort: port,
            configuration: receiverConfiguration,
            fixedCallbackSize: true
        )
        receiver.setMonitorPair(startingAtOneBased: 1)

        var senderConfiguration = AOOStreamConfiguration
            .adaptiveWireless(channelCount: 2)
        senderConfiguration.maximumCallbackFrames = 64
        let sender = try AOOSender(configuration: senderConfiguration)
        try sender.apply(AOOPeerConfiguration(
            isEnabled: true,
            host: "127.0.0.1",
            receiverPort: port
        ))

        let peak = pump(
            sender: sender,
            receiver: receiver,
            sourceChannelCount: 2,
            sourceLeftChannel: 0,
            sourceRightChannel: 1,
            frameCount: 64,
            iterations: 300
        )
        let status = receiver.status
        #expect(peak == 0)
        #expect(!status.streamActive)
        #expect(status.incompatibleStreamCount > 0)
        #expect(status.lastErrorCode != 0)
    }

    @Test func activeReceiverCanBeDestroyedWithoutStoppingTheSender() throws {
        for iteration in 0..<16 {
            let port = testPort(offset: 20 + iteration)
            var receiverConfiguration = AOOStreamConfiguration
                .automaticReceiver(channelCapacity: 2)
            receiverConfiguration.maximumCallbackFrames = 64
            var receiver: AOOReceiver? = try AOOReceiver(
                localPort: port,
                configuration: receiverConfiguration,
                fixedCallbackSize: true
            )
            receiver?.setMonitorPair(startingAtOneBased: 1)

            var senderConfiguration = AOOStreamConfiguration
                .deterministicWired(channelCount: 2)
            senderConfiguration.maximumCallbackFrames = 64
            let sender = try AOOSender(configuration: senderConfiguration)
            try sender.apply(AOOPeerConfiguration(
                isEnabled: true,
                host: "127.0.0.1",
                receiverPort: port
            ))

            let peak = pump(
                sender: sender,
                receiver: receiver!,
                sourceChannelCount: 2,
                sourceLeftChannel: 0,
                sourceRightChannel: 1,
                frameCount: 64,
                iterations: 900
            )
            #expect(peak > 0.05)
            #expect(waitUntil { receiver?.status.streamActive == true })

            receiver = nil
            Thread.sleep(forTimeInterval: 0.002)
        }
    }

    @Test func deterministicWiredReacquiresAfterASequenceGap() throws {
        let port = testPort(offset: 40)
        var receiverConfiguration = AOOStreamConfiguration
            .automaticReceiver(channelCapacity: 2)
        receiverConfiguration.maximumCallbackFrames = 64
        let receiver = try AOOReceiver(
            localPort: port,
            configuration: receiverConfiguration,
            fixedCallbackSize: true
        )
        receiver.setMonitorPair(startingAtOneBased: 1)

        var senderConfiguration = AOOStreamConfiguration
            .deterministicWired(channelCount: 2)
        senderConfiguration.maximumCallbackFrames = 64
        let sender = try AOOSender(configuration: senderConfiguration)
        try sender.apply(AOOPeerConfiguration(
            isEnabled: true,
            host: "127.0.0.1",
            receiverPort: port
        ))

        _ = pump(
            sender: sender,
            receiver: receiver,
            sourceChannelCount: 2,
            sourceLeftChannel: 0,
            sourceRightChannel: 1,
            frameCount: 64,
            iterations: 900
        )
        #expect(waitUntil { receiver.status.streamActive })

        try sender.setSimulatedPacketLoss(1)
        _ = pump(
            sender: sender,
            receiver: receiver,
            sourceChannelCount: 2,
            sourceLeftChannel: 0,
            sourceRightChannel: 1,
            frameCount: 64,
            iterations: 8,
            stopAfterSignal: false
        )
        try sender.setSimulatedPacketLoss(0)

        let peak = pump(
            sender: sender,
            receiver: receiver,
            sourceChannelCount: 2,
            sourceLeftChannel: 0,
            sourceRightChannel: 1,
            frameCount: 64,
            iterations: 1_200,
            stopAfterSignal: false
        )
        #expect(peak > 0.05)
        #expect(waitUntil(timeout: 1) { receiver.status.streamActive })
        #expect(receiver.status.reacquisitionCount > 0)
        #expect(abs(receiver.status.jitterBufferLatencyMilliseconds - 4) < 0.1)
        #expect(receiver.status.processErrorCount == 0)
    }

    private func pump(
        sender: AOOSender,
        receiver: AOOReceiver,
        sourceChannelCount: Int,
        sourceLeftChannel: Int,
        sourceRightChannel: Int,
        frameCount: Int,
        iterations: Int,
        injectNonfiniteSamples: Bool = false,
        stopAfterSignal: Bool = true
    ) -> Float {
        var input = Array(
            repeating: Float.zero,
            count: frameCount * sourceChannelCount
        )
        for frame in 0..<frameCount {
            input[sourceLeftChannel * frameCount + frame] = 0.25
            input[sourceRightChannel * frameCount + frame] = -0.125
        }
        if injectNonfiniteSamples, frameCount >= 2 {
            input[sourceLeftChannel * frameCount] = .nan
            input[sourceRightChannel * frameCount + 1] = .infinity
        }
        var outputLeft = Array(repeating: Float.zero, count: frameCount)
        var outputRight = Array(repeating: Float.zero, count: frameCount)
        var peak: Float = 0
        let blockDuration = Double(frameCount) / sender.sampleRate

        for _ in 0..<iterations {
            if stopAfterSignal, peak > 0.05 {
                break
            }
            input.withUnsafeBufferPointer { input in
                sender.processPlanar(
                    baseAddress: input.baseAddress!,
                    channelStride: frameCount,
                    frameOffset: 0,
                    frameCount: frameCount,
                    sourceChannelCount: sourceChannelCount
                )
            }
            outputLeft.withUnsafeMutableBufferPointer { left in
                outputRight.withUnsafeMutableBufferPointer { right in
                    receiver.processStereo(
                        outputLeft: left.baseAddress!,
                        outputRight: right.baseAddress!,
                        frameCount: frameCount
                    )
                }
            }
            #expect(outputLeft.allSatisfy { $0.isFinite })
            #expect(outputRight.allSatisfy { $0.isFinite })
            peak = max(
                peak,
                outputLeft.reduce(Float.zero) { max($0, abs($1)) },
                outputRight.reduce(Float.zero) { max($0, abs($1)) }
            )
            Thread.sleep(forTimeInterval: blockDuration)
        }
        return peak
    }

    private func waitUntil(
        timeout: TimeInterval = 0.1,
        condition: () -> Bool
    ) -> Bool {
        let deadline = Date().addingTimeInterval(timeout)
        repeat {
            if condition() {
                return true
            }
            Thread.sleep(forTimeInterval: 0.002)
        } while Date() < deadline
        return condition()
    }

    private func testPort(offset: Int) -> Int {
        24_000 + Int(getpid() % 20_000) + offset
    }
}
