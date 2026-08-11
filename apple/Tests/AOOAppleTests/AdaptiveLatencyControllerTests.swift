import Testing
@testable import AOOApple

@Test func wirelessLatencyStartsFromMeasuredRoundTripTime() {
    let controller = AOOAdaptiveLatencyController(
        sampleRate: 48_000,
        blockSize: 64,
        initialRoundTripP99Milliseconds: 12
    )
    let expectedMinimum = 6.0 + 2.0 * 64.0 / 48_000.0 * 1_000.0
    #expect(controller.targetLatencyMilliseconds >= expectedMinimum)
    let blocks = controller.targetLatencyMilliseconds
        / controller.blockDurationMilliseconds
    #expect(abs(blocks.rounded() - blocks) < 0.000_001)
}

@Test func deficitRaisesTargetImmediatelyWithSafetyBlocks() {
    var controller = AOOAdaptiveLatencyController(
        sampleRate: 48_000,
        blockSize: 64
    )
    let previous = controller.targetLatencyMilliseconds
    let observedJitter = previous + 7
    let decision = controller.observe(
        arrivalResidualMilliseconds: observedJitter,
        wasLate: true,
        concealed: false,
        underrun: false,
        now: 1
    )
    #expect(decision.changed)
    #expect(decision.reason == .deficit)
    #expect(decision.targetLatencyMilliseconds
        >= observedJitter + 2 * controller.blockDurationMilliseconds)
}

@Test func stableStreamDecreasesOncePerThirtySeconds() {
    var controller = AOOAdaptiveLatencyController(
        sampleRate: 48_000,
        blockSize: 64
    )
    let initial = controller.targetLatencyMilliseconds
    _ = controller.observe(
        arrivalResidualMilliseconds: -10,
        wasLate: false,
        concealed: false,
        underrun: false,
        now: 0
    )
    let beforeInterval = controller.observe(
        arrivalResidualMilliseconds: -10,
        wasLate: false,
        concealed: false,
        underrun: false,
        now: 29.9
    )
    #expect(!beforeInterval.changed)

    let firstDecrease = controller.observe(
        arrivalResidualMilliseconds: -10,
        wasLate: false,
        concealed: false,
        underrun: false,
        now: 30
    )
    #expect(firstDecrease.changed)
    #expect(firstDecrease.reason == .sustainedHeadroom)
    #expect(firstDecrease.targetLatencyMilliseconds < initial)

    let tooSoon = controller.observe(
        arrivalResidualMilliseconds: -10,
        wasLate: false,
        concealed: false,
        underrun: false,
        now: 59.9
    )
    #expect(!tooSoon.changed)
}

@Test func instabilityRestartsDecreaseInterval() {
    var controller = AOOAdaptiveLatencyController(
        sampleRate: 48_000,
        blockSize: 64
    )
    _ = controller.observe(
        arrivalResidualMilliseconds: -10,
        wasLate: false,
        concealed: false,
        underrun: false,
        now: 0
    )
    _ = controller.observe(
        arrivalResidualMilliseconds: 2,
        wasLate: true,
        concealed: false,
        underrun: false,
        now: 20
    )
    let tooSoon = controller.observe(
        arrivalResidualMilliseconds: -10,
        wasLate: false,
        concealed: false,
        underrun: false,
        now: 49
    )
    #expect(!tooSoon.changed)
}

@Test func resendRequiresPredictedArrivalBeforeDeadline() {
    let controller = AOOAdaptiveLatencyController(
        sampleRate: 48_000,
        blockSize: 64
    )
    #expect(controller.shouldRequestResend(
        millisecondsUntilDeadline: 10,
        roundTripP95Milliseconds: 5
    ))
    #expect(!controller.shouldRequestResend(
        millisecondsUntilDeadline: 7,
        roundTripP95Milliseconds: 5
    ))
}
