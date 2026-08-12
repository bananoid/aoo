import Darwin
import Testing
@testable import AOOApple

@Test func machHostTimeMapsOntoCurrentNTPClock() {
    var timebase = mach_timebase_info_data_t()
    mach_timebase_info(&timebase)
    let currentHostTime = mach_absolute_time()
    let oneSecondInTicks = UInt64(1_000_000_000) * UInt64(timebase.denom)
        / UInt64(timebase.numer)

    let current = AOOAudioClock.ntpTime(forMachHostTime: currentHostTime)
    let previous = AOOAudioClock.ntpTime(
        forMachHostTime: currentHostTime - oneSecondInTicks
    )
    let currentWallClock = AOOAudioClock.currentNTPTime()

    #expect(abs(ntpSeconds(currentWallClock) - ntpSeconds(current)) < 0.05)
    #expect(abs((ntpSeconds(current) - ntpSeconds(previous)) - 1) < 0.001)
}

private func ntpSeconds(_ timestamp: UInt64) -> Double {
    let seconds = Double(timestamp >> 32)
    let fraction = Double(timestamp & 0xffff_ffff) / 4_294_967_296
    return seconds + fraction
}
