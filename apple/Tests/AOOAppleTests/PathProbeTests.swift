import Testing
@testable import AOOApple

@Test func pathProbeUsesNegotiatedPCMWidth() {
    let floatReport = AOOPathProbeReport(
        durationSeconds: 1,
        senderStatus: AOOSenderStatus(
            peerResponsive: true,
            channelCount: 20,
            sampleRate: 48_000,
            streamBlockSize: 64,
            packetSize: 1_400,
            profile: .deterministicWired,
            format: .float32
        ),
        roundTripSamplesMilliseconds: Array(repeating: 1, count: 20),
        packetLossSamples: [0]
    )
    let int24Report = AOOPathProbeReport(
        durationSeconds: 1,
        senderStatus: AOOSenderStatus(
            peerResponsive: true,
            channelCount: 20,
            sampleRate: 48_000,
            streamBlockSize: 64,
            packetSize: 1_200,
            profile: .adaptiveWireless,
            format: .int24
        ),
        roundTripSamplesMilliseconds: Array(repeating: 1, count: 20),
        packetLossSamples: [0]
    )

    #expect(floatReport.audioPayloadBytesPerBlock == 20 * 64 * 4)
    #expect(int24Report.audioPayloadBytesPerBlock == 20 * 64 * 3)
    #expect(int24Report.rawAudioPayloadMegabitsPerSecond
        == floatReport.rawAudioPayloadMegabitsPerSecond * 0.75)
}
