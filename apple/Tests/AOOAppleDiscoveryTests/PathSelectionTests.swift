import Foundation
import Testing
import AOOApple
@testable import AOOAppleDiscovery

private let receiver = AOOPeerDescriptor(
    id: UUID(uuidString: "00000000-0000-0000-0000-000000000001")!,
    aooPort: 9_999,
    sampleRate: 48_000,
    channelCount: 20
)

private func path(
    host: String,
    kind: AOOInterfaceKind,
    p95: Double,
    p99: Double? = nil,
    loss: Double = 0
) -> AOOPathCandidate {
    AOOPathCandidate(
        receiver: receiver,
        host: host,
        interfaceName: kind.rawValue,
        interfaceIndex: host.hashValue,
        interfaceKind: kind,
        medianRoundTripMilliseconds: p95 * 0.8,
        p95RoundTripMilliseconds: p95,
        p99RoundTripMilliseconds: p99 ?? p95 * 1.1,
        packetLoss: loss,
        successfulProbeCount: 8,
        requestedProbeCount: 8
    )
}

@Test func directPathWinsOverFasterWifi() {
    let direct = path(host: "direct", kind: .direct, p95: 3)
    let wifi = path(host: "wifi", kind: .wifi, p95: 1)
    #expect(AOOPathSelectionPolicy.bestPath(in: [wifi, direct]) == direct)
}

@Test func directIPv4WinsOverFasterIPv6OnTheSameInterface() {
    let ipv4 = path(host: "169.254.12.81", kind: .direct, p95: 2)
    let ipv6 = path(host: "fe80::1234%en6", kind: .direct, p95: 0.5)
    #expect(AOOPathSelectionPolicy.bestPath(in: [ipv6, ipv4]) == ipv4)
}

@Test func scopedIPv6RemainsAvailableAsFallback() {
    let ipv6 = path(host: "fe80::1234%en6", kind: .direct, p95: 1)
    #expect(AOOPathSelectionPolicy.bestPath(in: [ipv6]) == ipv6)
}

@Test func activeStreamNeverSilentlySwitchesFromViablePath() {
    let current = path(host: "wifi", kind: .wifi, p95: 8)
    let better = path(host: "direct", kind: .direct, p95: 1)
    #expect(AOOPathSelectionPolicy.selectedPath(
        in: [current, better],
        current: current,
        streamIsActive: true
    ) == current)
    #expect(AOOPathSelectionPolicy.selectedPath(
        in: [current, better],
        current: current,
        streamIsActive: false
    ) == better)
}

@Test func failedActivePathMayBeReplaced() {
    var current = path(host: "wifi", kind: .wifi, p95: 8)
    current.successfulProbeCount = 0
    let replacement = path(host: "direct", kind: .direct, p95: 2)
    #expect(AOOPathSelectionPolicy.selectedPath(
        in: [current, replacement],
        current: current,
        streamIsActive: true
    ) == replacement)
}
