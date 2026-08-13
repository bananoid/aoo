import Testing
@testable import AOOApple

@Test func deterministicWiredDefaultsAndValidation() throws {
    let configuration = try AOOStreamConfiguration
        .deterministicWired(channelCount: 20)
        .validated()

    #expect(configuration.profile == .deterministicWired)
    #expect(configuration.format == .float32)
    #expect(configuration.blockSize == 64)
    #expect(configuration.datagramSize == 1_400)
    #expect(
        abs(
            configuration.targetLatencyMilliseconds
                - 4 * configuration.blockDurationMilliseconds
        ) < 0.000_001
    )
    #expect(configuration.channelMap == Array(0..<20))
    #expect(configuration.capabilities == .required)
}

@Test func adaptiveWirelessDefaultsUseWholeBlocks() throws {
    let configuration = try AOOStreamConfiguration
        .adaptiveWireless(
            channelCount: 8,
            initialRoundTripP99Milliseconds: 20
        )
        .validated()

    #expect(configuration.profile == .adaptiveWireless)
    #expect(configuration.format == .int24)
    #expect(configuration.blockSize == 64)
    #expect(configuration.datagramSize == 1_200)
    #expect(configuration.capabilities.contains(.deadlineResend))
    let blocks = configuration.targetLatencyMilliseconds
        / configuration.blockDurationMilliseconds
    #expect(abs(blocks.rounded() - blocks) < 0.000_001)
}

@Test func automaticProfileMustBeResolvedBeforeStart() {
    let configuration = AOOStreamConfiguration(
        profile: .automatic,
        format: .float32,
        channelCount: 2,
        sampleRate: 48_000,
        datagramSize: 1_400,
        targetLatencyMilliseconds: 4
    )
    #expect(throws: AOOTransportError.self) {
        try configuration.validated()
    }
}

@Test func automaticReceiverAdvertisesBothProfiles() throws {
    let configuration = try AOOStreamConfiguration
        .automaticReceiver(channelCapacity: 20)
        .validatedForReceiver()

    #expect(configuration.profile == .automatic)
    #expect(configuration.channelCount == 20)
    #expect(configuration.blockSize == 64)
    #expect(configuration.datagramSize == 1_400)
    #expect(configuration.capabilities.contains(.required))
    #expect(configuration.capabilities.contains(.deadlineResend))
}

@Test func profileFormatMismatchIsRejected() {
    var configuration = AOOStreamConfiguration.deterministicWired(channelCount: 2)
    configuration.format = .int24
    #expect(throws: AOOTransportError.self) {
        try configuration.validated()
    }
}

@Test func channelMapMustBeUnique() {
    var configuration = AOOStreamConfiguration.deterministicWired(channelCount: 2)
    configuration.channelMap = [0, 0]
    #expect(throws: AOOTransportError.self) {
        try configuration.validated()
    }
}
