# Low-Latency Stream Profiles

The `low-latency` branch adds a versioned PCM stream protocol and Apple
package. It is intentionally not wire-compatible with stock AOO peers. The
regular `master` and `develop` branches remain the compatibility paths.

## Protocol

Each stream starts with an `AOLS` configuration record. It fixes the protocol
version, profile, PCM format, channel map, sample rate, block size, datagram
size, target depth, and capability bits for the lifetime of the stream. A
configuration change stops the current stream and starts another stream with a
new stream ID.

Each audio fragment carries an `AOLL` header with:

- protocol version and header size;
- profile and PCM format;
- stream ID and wrapping 32-bit sequence number;
- absolute source sample position and source NTP timestamp;
- block size, fragment index/count, channel count, and payload size.

Peers reject malformed records, unsupported capabilities, profile/format
mismatches, and disagreements between the configuration metadata and the AOO
PCM codec format. Late data is not inserted behind the receiver's monotonic
playout position.

## Deterministic Wired

`deterministicWired` is intended for direct Ethernet and USB network paths.
Its defaults are:

- Float32 PCM;
- 64-frame source blocks;
- 1,400-byte datagrams;
- no redundancy or resend history;
- a three-block minimum, rounded up to a 4 ms target.

The source audio callback copies into a preallocated SPSC queue and publishes
metadata atomically. One deadline-paced worker consumes that queue, calls the
AOO source processor, and sends packets. If the audio producer overruns the
main queue, a bounded gap queue preserves the absolute sample timeline by
emitting silence rather than compressing time.

At the receiver, an isolated missing block produces silence while sequence and
sample position continue forward. Four consecutive missing blocks enter
reacquisition. The receiver then waits for a complete consecutive prefix at
the fixed target depth. If queued delivery resumes with a larger backlog, the
receiver drops the oldest complete blocks before playout so the backlog cannot
become additional latency. It then discards stale packets and resumes without
moving its sample position backward. Dynamic resampling remains enabled only
for slow independent-device clock drift.

## Adaptive Wireless

`adaptiveWireless` is intended for Wi-Fi and other variable-delay paths. Its
defaults are:

- packed 24-bit PCM;
- 64-frame source blocks;
- 1,200-byte datagrams;
- an initial target of `max(8 ms, p99 RTT / 2 + two blocks)`, rounded to whole
  blocks, or 50 ms when no usable probe exists.

The sink retains a fixed 4,096-observation arrival window. A deficit raises
the target enough to cover p99 arrival jitter plus two blocks. Deficit-driven
increases are immediate. A decrease is limited to one block after 30 seconds
with no late packet, concealment, or underrun and at least three blocks of p99
headroom. Decreases occur at most once per 30-second interval, and the target
is bounded to 200 ms.

A missing block requests at most one resend, and only when p95 RTT plus two
blocks is earlier than the predicted playout deadline. Packets that miss the
deadline are discarded. Wireless loss uses the same monotonic concealment and
reacquisition rules as the wired profile. XOR FEC is not part of this version.

## Apple Package

The root Swift package supports iOS 16 and macOS 13 or later and exports:

- `AOOApple`: stream configuration, sender, receiver, timing, telemetry,
  adaptive latency, channel mapping, and path probes;
- `AOOAppleDiscovery`: Bonjour advertisement/browsing and path selection.

Bonjour uses `_aoo-audio._udp`. TXT records contain only protocol version,
receiver UUID, UDP port, sample rate, channel count, supported profiles,
formats, and block sizes. The default service name is `AOO Receiver`.

Automatic selection prefers viable direct or wired paths, followed by other
paths and Wi-Fi. RTT tail spread and loss break ties within an interface class.
Each interface is probed over IPv4 and IPv6; IPv4 is preferred within the same
interface class, while scoped IPv6 remains available when no viable IPv4 path
exists.
An active viable stream never silently switches to a newly discovered path;
the better path is considered after streaming stops. A failed route requires
an explicit stream restart.

## Realtime Contract

Constructing, configuring, changing destinations, changing target latency,
and reading rich status are non-realtime operations. Once configured:

- sender and receiver audio callbacks perform bounded copies, arithmetic, and
  atomic publication only;
- audio callbacks do not allocate, lock, sleep, perform DNS, or call Bonjour;
- packet I/O, event handling, adaptation, and status aggregation run outside
  the audio callback;
- callbacks may use arbitrary host block sizes up to the configured maximum
  unless `fixedCallbackSize` is enabled; fixed mode requires every receiver
  callback to equal `maximumCallbackFrames` exactly.

Apple render callbacks should pass the sample timeline from Core Audio rather
than the wall-clock time at callback entry. `AOOAudioClock.ntpTime(forMachHostTime:)`
maps `AudioTimeStamp.mHostTime` onto AOO's NTP clock for this purpose.

The public API reports handoff drops, UDP datagram attempts and failures, late
packets, concealments, resends, reacquisitions, underruns, overruns, arrival
residuals, receive-datagram gaps, stale fragments, incomplete blocks, trimmed
backlog, clock drift, buffer fill, target latency, and timestamp-derived
presentation latency. Applications should validate these metrics on their
target hardware and network; the default targets are starting points, not
latency guarantees.

## Build And Test

```sh
cmake -S . -B build-low-latency \
  -DAOO_USE_OPUS=OFF \
  -DAOO_BUILD_TESTS=ON \
  -DAOO_BUILD_EXAMPLES=OFF
cmake --build build-low-latency
ctest --test-dir build-low-latency --output-on-failure
swift test
scripts/check-public-privacy.sh
```

UDP loopback tests require the test process to be allowed to open local
datagram sockets.
