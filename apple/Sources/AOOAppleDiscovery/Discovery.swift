import AOOApple
import Foundation
import Network

public enum AOODiscoveryConstants {
    public static let serviceType = "_aoo-audio._udp"
    public static let defaultServiceName = "AOO Receiver"
    public static let protocolVersion = AOOStreamConfiguration.protocolVersion
    public static let probeCount = 8
}

public struct AOOPeerDescriptor: Codable, Equatable, Sendable {
    public var id: UUID
    public var name: String
    public var aooPort: Int
    public var sampleRate: Int
    public var channelCount: Int
    public var supportedProfiles: [AOOTransportProfile]
    public var supportedFormats: [AOOAudioFormat]
    public var supportedBlockSizes: [Int]

    public init(
        id: UUID,
        name: String = AOODiscoveryConstants.defaultServiceName,
        aooPort: Int,
        sampleRate: Int,
        channelCount: Int,
        supportedProfiles: [AOOTransportProfile] = [
            .deterministicWired,
            .adaptiveWireless
        ],
        supportedFormats: [AOOAudioFormat] = [.float32, .int24],
        supportedBlockSizes: [Int] = [64]
    ) {
        self.id = id
        self.name = name.trimmingCharacters(in: .whitespacesAndNewlines)
        self.aooPort = AOOPeerConfiguration.clampedPort(aooPort)
        self.sampleRate = max(0, sampleRate)
        self.channelCount = max(0, channelCount)
        self.supportedProfiles = supportedProfiles.filter { $0 != .automatic }
        self.supportedFormats = supportedFormats
        self.supportedBlockSizes = supportedBlockSizes
            .filter { $0 > 0 }
            .sorted()
    }
}

public enum AOOInterfaceKind: String, Codable, Equatable, Sendable {
    case direct
    case wired
    case wifi
    case other

    public var displayName: String {
        switch self {
        case .direct: "Direct"
        case .wired: "Wired"
        case .wifi: "Wi-Fi"
        case .other: "Other"
        }
    }

    public var recommendedProfile: AOOTransportProfile {
        switch self {
        case .direct, .wired:
            return .deterministicWired
        case .wifi, .other:
            return .adaptiveWireless
        }
    }
}

public struct AOOPathCandidate: Codable, Equatable, Identifiable, Sendable {
    public var receiver: AOOPeerDescriptor
    public var host: String
    public var interfaceName: String
    public var interfaceIndex: Int
    public var interfaceKind: AOOInterfaceKind
    public var medianRoundTripMilliseconds: Double
    public var p95RoundTripMilliseconds: Double
    public var p99RoundTripMilliseconds: Double
    public var packetLoss: Double
    public var successfulProbeCount: Int
    public var requestedProbeCount: Int

    public init(
        receiver: AOOPeerDescriptor,
        host: String,
        interfaceName: String,
        interfaceIndex: Int,
        interfaceKind: AOOInterfaceKind,
        medianRoundTripMilliseconds: Double,
        p95RoundTripMilliseconds: Double,
        p99RoundTripMilliseconds: Double,
        packetLoss: Double,
        successfulProbeCount: Int,
        requestedProbeCount: Int
    ) {
        self.receiver = receiver
        self.host = host
        self.interfaceName = interfaceName
        self.interfaceIndex = interfaceIndex
        self.interfaceKind = interfaceKind
        self.medianRoundTripMilliseconds = medianRoundTripMilliseconds
        self.p95RoundTripMilliseconds = p95RoundTripMilliseconds
        self.p99RoundTripMilliseconds = p99RoundTripMilliseconds
        self.packetLoss = min(max(packetLoss.isFinite ? packetLoss : 1, 0), 1)
        self.successfulProbeCount = max(0, successfulProbeCount)
        self.requestedProbeCount = max(0, requestedProbeCount)
    }

    public var id: String {
        "\(receiver.id.uuidString)|\(interfaceIndex)|\(host)"
    }

    public var isViable: Bool {
        successfulProbeCount >= 3
            && packetLoss <= 0.25
            && medianRoundTripMilliseconds.isFinite
            && p95RoundTripMilliseconds.isFinite
            && p99RoundTripMilliseconds.isFinite
    }

    public var displayName: String {
        let interfaceLabel = interfaceName.isEmpty
            ? interfaceKind.displayName
            : "\(interfaceKind.displayName) (\(interfaceName))"
        return String(format: "%@  %.2f ms p95", interfaceLabel, p95RoundTripMilliseconds)
    }

    fileprivate var selectionCost: Double {
        guard isViable else { return .infinity }
        let tailSpread = max(0, p99RoundTripMilliseconds - medianRoundTripMilliseconds)
        return p95RoundTripMilliseconds + tailSpread * 0.5 + packetLoss * 1_000
    }
}

public struct AOODiscoveredPeer: Codable, Equatable, Identifiable, Sendable {
    public var descriptor: AOOPeerDescriptor
    public var paths: [AOOPathCandidate]

    public init(
        descriptor: AOOPeerDescriptor,
        paths: [AOOPathCandidate]
    ) {
        self.descriptor = descriptor
        self.paths = paths
    }

    public var id: UUID { descriptor.id }

    public func bestPath(currentHost: String? = nil) -> AOOPathCandidate? {
        AOOPathSelectionPolicy.bestPath(in: paths, currentHost: currentHost)
    }
}

public enum AOOPathSelectionPolicy {
    public static func bestPath(
        in paths: [AOOPathCandidate],
        currentHost: String? = nil
    ) -> AOOPathCandidate? {
        let viable = paths.filter(\.isViable).sorted(by: isPreferred)
        guard let best = viable.first else { return nil }
        guard let currentHost,
              let current = viable.first(where: { $0.host == currentHost }),
              current.id != best.id else {
            return best
        }

        let bestInterfacePreference = interfacePreference(best.interfaceKind)
        let currentInterfacePreference = interfacePreference(current.interfaceKind)
        if bestInterfacePreference < currentInterfacePreference {
            return best
        }
        if bestInterfacePreference == currentInterfacePreference,
           addressPreference(best.host) < addressPreference(current.host) {
            return best
        }

        let requiredImprovement = max(0.5, current.selectionCost * 0.15)
        return best.selectionCost + requiredImprovement < current.selectionCost
            ? best
            : current
    }

    public static func selectedPath(
        in paths: [AOOPathCandidate],
        current: AOOPathCandidate?,
        streamIsActive: Bool
    ) -> AOOPathCandidate? {
        if streamIsActive, let current, current.isViable {
            return current
        }
        return bestPath(in: paths, currentHost: current?.host)
    }

    private static func isPreferred(
        _ lhs: AOOPathCandidate,
        _ rhs: AOOPathCandidate
    ) -> Bool {
        let kindDifference = interfacePreference(lhs.interfaceKind)
            - interfacePreference(rhs.interfaceKind)
        if kindDifference != 0 {
            return kindDifference < 0
        }
        let addressDifference = addressPreference(lhs.host)
            - addressPreference(rhs.host)
        if addressDifference != 0 {
            return addressDifference < 0
        }
        let costDifference = lhs.selectionCost - rhs.selectionCost
        if abs(costDifference) > 0.25 {
            return costDifference < 0
        }
        if lhs.p95RoundTripMilliseconds != rhs.p95RoundTripMilliseconds {
            return lhs.p95RoundTripMilliseconds < rhs.p95RoundTripMilliseconds
        }
        return lhs.id < rhs.id
    }

    private static func interfacePreference(_ kind: AOOInterfaceKind) -> Int {
        switch kind {
        case .direct, .wired: 0
        case .other: 2
        case .wifi: 3
        }
    }

    private static func addressPreference(_ host: String) -> Int {
        let unwrapped = host.trimmingCharacters(
            in: CharacterSet(charactersIn: "[]")
        )
        let address = unwrapped.split(
            separator: "%",
            maxSplits: 1,
            omittingEmptySubsequences: false
        ).first.map(String.init) ?? unwrapped
        let octets = address.split(
            separator: ".",
            omittingEmptySubsequences: false
        )
        if octets.count == 4,
           octets.allSatisfy({ octet in
               guard let value = Int(octet) else { return false }
               return (0...255).contains(value)
           }) {
            return 0
        }
        return address.contains(":") ? 1 : 2
    }
}

public enum AOODiscoveryState: String, Codable, Equatable, Sendable {
    case stopped
    case searching
    case ready
    case failed
}

public struct AOOPeerBrowserSnapshot: Codable, Equatable, Sendable {
    public var state: AOODiscoveryState
    public var receivers: [AOODiscoveredPeer]
    public var errorMessage: String?

    public init(
        state: AOODiscoveryState = .stopped,
        receivers: [AOODiscoveredPeer] = [],
        errorMessage: String? = nil
    ) {
        self.state = state
        self.receivers = receivers
        self.errorMessage = errorMessage
    }
}

public enum AOOServiceAdvertiserState: Equatable, Sendable {
    case stopped
    case advertising
    case failed(String)
}

private struct AOOProbeRequest: Codable {
    var version: Int
    var nonce: UInt64
}

private struct AOOProbeResponse: Codable {
    var version: Int
    var nonce: UInt64
    var receiver: AOOPeerDescriptor
    var receiverHost: String
}

public final class AOOServiceAdvertiser: @unchecked Sendable {
    public typealias StateHandler = @Sendable (AOOServiceAdvertiserState) -> Void

    private let queue = DispatchQueue(
        label: "AOOAppleDiscovery.ReceiverAdvertiser",
        qos: .utility
    )
    private let lock = NSLock()
    private var listener: NWListener?
    private var connections: [UUID: NWConnection] = [:]
    private var descriptor: AOOPeerDescriptor?
    private var stateHandler: StateHandler?

    public init() {}

    deinit {
        stop()
    }

    public func start(
        descriptor: AOOPeerDescriptor,
        stateHandler: StateHandler? = nil
    ) throws {
        stop()
        let listener = try NWListener(using: .udp)
        listener.service = NWListener.Service(
            name: descriptor.name,
            type: AOODiscoveryConstants.serviceType,
            txtRecord: Self.txtRecord(for: descriptor)
        )
        listener.newConnectionHandler = { [weak self] connection in
            self?.accept(connection)
        }
        listener.stateUpdateHandler = { [weak self] state in
            self?.handle(listenerState: state)
        }
        lock.withLock {
            self.descriptor = descriptor
            self.stateHandler = stateHandler
            self.listener = listener
        }
        listener.start(queue: queue)
    }

    public func stop() {
        let resources = lock.withLock { () -> (NWListener?, [NWConnection], StateHandler?) in
            let resources = (listener, Array(connections.values), stateHandler)
            listener = nil
            connections.removeAll()
            descriptor = nil
            stateHandler = nil
            return resources
        }
        resources.0?.cancel()
        resources.1.forEach { $0.cancel() }
        resources.2?(.stopped)
    }

    private func accept(_ connection: NWConnection) {
        let connectionID = UUID()
        lock.withLock {
            connections[connectionID] = connection
        }
        connection.stateUpdateHandler = { [weak self, weak connection] state in
            guard let self, let connection else { return }
            switch state {
            case .ready:
                self.receiveNext(on: connection, id: connectionID)
            case .cancelled, .failed:
                self.removeConnection(id: connectionID)
            default:
                break
            }
        }
        connection.start(queue: queue)
    }

    private func receiveNext(on connection: NWConnection, id: UUID) {
        connection.receiveMessage { [weak self, weak connection] data, _, _, error in
            guard let self, let connection else { return }
            if error != nil {
                self.removeConnection(id: id)
                return
            }
            if let data,
               let request = try? JSONDecoder().decode(AOOProbeRequest.self, from: data),
               request.version == AOODiscoveryConstants.protocolVersion,
               let descriptor = self.lock.withLock({ self.descriptor }) {
                let response = AOOProbeResponse(
                    version: AOODiscoveryConstants.protocolVersion,
                    nonce: request.nonce,
                    receiver: descriptor,
                    receiverHost: Self.hostString(
                        from: connection.currentPath?.localEndpoint
                    ) ?? ""
                )
                if let responseData = try? JSONEncoder().encode(response) {
                    connection.send(
                        content: responseData,
                        completion: .contentProcessed { _ in }
                    )
                }
            }
            self.receiveNext(on: connection, id: id)
        }
    }

    private func removeConnection(id: UUID) {
        let connection = lock.withLock { connections.removeValue(forKey: id) }
        connection?.cancel()
    }

    private func handle(listenerState: NWListener.State) {
        let handler = lock.withLock { stateHandler }
        switch listenerState {
        case .ready:
            handler?(.advertising)
        case let .failed(error):
            handler?(.failed(error.localizedDescription))
        case .cancelled:
            handler?(.stopped)
        default:
            break
        }
    }

    private static func txtRecord(for descriptor: AOOPeerDescriptor) -> Data {
        let entries = [
            "v=\(AOODiscoveryConstants.protocolVersion)",
            "id=\(descriptor.id.uuidString)",
            "port=\(descriptor.aooPort)",
            "rate=\(descriptor.sampleRate)",
            "channels=\(descriptor.channelCount)",
            "profiles=\(descriptor.supportedProfiles.map(\.rawValue).map(String.init).joined(separator: ","))",
            "formats=\(descriptor.supportedFormats.map(\.rawValue).map(String.init).joined(separator: ","))",
            "blocks=\(descriptor.supportedBlockSizes.map(String.init).joined(separator: ","))"
        ]
        var data = Data()
        for entry in entries {
            let bytes = Array(entry.utf8.prefix(255))
            data.append(UInt8(bytes.count))
            data.append(contentsOf: bytes)
        }
        return data
    }

    private static func hostString(from endpoint: NWEndpoint?) -> String? {
        guard case let .hostPort(host, _) = endpoint else { return nil }
        return host.debugDescription
    }
}

public final class AOOPeerBrowser: @unchecked Sendable {
    public typealias SnapshotHandler = @Sendable (AOOPeerBrowserSnapshot) -> Void

    private let queue = DispatchQueue(
        label: "AOOAppleDiscovery.ReceiverDiscovery",
        qos: .utility
    )
    private var browser: NWBrowser?
    private var probeSessions: [AOOPathProbeSession] = []
    private var candidates: [String: AOOPathCandidate] = [:]
    private var remainingProbeCount = 0
    private var snapshotHandler: SnapshotHandler?
    private var generation: UInt64 = 0

    public init() {}

    deinit {
        browser?.cancel()
        probeSessions.forEach { $0.cancel() }
    }

    public func start(snapshotHandler: @escaping SnapshotHandler) {
        queue.async { [weak self] in
            guard let self else { return }
            self.snapshotHandler = snapshotHandler
            self.restartBrowser()
        }
    }

    public func refresh() {
        queue.async { [weak self] in
            self?.restartBrowser()
        }
    }

    public func stop() {
        queue.async { [weak self] in
            guard let self else { return }
            self.generation &+= 1
            self.browser?.cancel()
            self.browser = nil
            self.probeSessions.forEach { $0.cancel() }
            self.probeSessions.removeAll()
            self.candidates.removeAll()
            self.remainingProbeCount = 0
            self.emit(state: .stopped)
            self.snapshotHandler = nil
        }
    }

    private func restartBrowser() {
        generation &+= 1
        browser?.cancel()
        probeSessions.forEach { $0.cancel() }
        probeSessions.removeAll()
        candidates.removeAll()
        remainingProbeCount = 0
        emit(state: .searching)

        let browser = NWBrowser(
            for: .bonjour(
                type: AOODiscoveryConstants.serviceType,
                domain: nil
            ),
            using: .udp
        )
        let browserGeneration = generation
        browser.browseResultsChangedHandler = { [weak self] results, _ in
            self?.probe(results: results, generation: browserGeneration)
        }
        browser.stateUpdateHandler = { [weak self] state in
            self?.handle(browserState: state, generation: browserGeneration)
        }
        self.browser = browser
        browser.start(queue: queue)
    }

    private func handle(browserState: NWBrowser.State, generation: UInt64) {
        guard generation == self.generation else { return }
        switch browserState {
        case .ready:
            if probeSessions.isEmpty {
                emit(state: .ready)
            }
        case let .failed(error):
            emit(state: .failed, errorMessage: error.localizedDescription)
        case .cancelled:
            break
        default:
            break
        }
    }

    private func probe(results: Set<NWBrowser.Result>, generation: UInt64) {
        guard generation == self.generation else { return }
        probeSessions.forEach { $0.cancel() }
        probeSessions.removeAll()
        candidates.removeAll()
        remainingProbeCount = 0
        guard !results.isEmpty else {
            emit(state: .ready)
            return
        }
        emit(state: .searching)

        remainingProbeCount = results.reduce(into: 0) { count, result in
            count += max(1, result.interfaces.count) * AOOProbeIPVersion.allCases.count
        }
        for result in results {
            let interfaces: [NWInterface?] = result.interfaces.isEmpty
                ? [nil]
                : result.interfaces.map(Optional.some)
            for interface in interfaces {
                for ipVersion in AOOProbeIPVersion.allCases {
                    let session = AOOPathProbeSession(
                        endpoint: result.endpoint,
                        interface: interface,
                        ipVersion: ipVersion,
                        queue: queue
                    ) { [weak self] candidate in
                        guard let self, generation == self.generation else { return }
                        if let candidate {
                            self.candidates[candidate.id] = candidate
                        }
                        self.remainingProbeCount = max(0, self.remainingProbeCount - 1)
                        self.emit(state: self.remainingProbeCount == 0 ? .ready : .searching)
                    }
                    probeSessions.append(session)
                    session.start()
                }
            }
        }
    }

    private func emit(
        state: AOODiscoveryState,
        errorMessage: String? = nil
    ) {
        let grouped = Dictionary(grouping: candidates.values, by: { $0.receiver.id })
        let receivers = grouped.values.compactMap { paths -> AOODiscoveredPeer? in
            guard let descriptor = paths.first?.receiver else { return nil }
            return AOODiscoveredPeer(
                descriptor: descriptor,
                paths: paths.sorted { $0.id < $1.id }
            )
        }.sorted {
            $0.descriptor.name.localizedCaseInsensitiveCompare($1.descriptor.name)
                == .orderedAscending
        }
        snapshotHandler?(AOOPeerBrowserSnapshot(
            state: state,
            receivers: receivers,
            errorMessage: errorMessage
        ))
    }
}

private enum AOOProbeIPVersion: CaseIterable {
    case v4
    case v6

    var networkVersion: NWProtocolIP.Options.Version {
        switch self {
        case .v4: .v4
        case .v6: .v6
        }
    }
}

private final class AOOPathProbeSession: @unchecked Sendable {
    typealias Completion = @Sendable (AOOPathCandidate?) -> Void

    private let endpoint: NWEndpoint
    private let interface: NWInterface?
    private let ipVersion: AOOProbeIPVersion
    private let queue: DispatchQueue
    private let completion: Completion
    private var connection: NWConnection?
    private var pendingProbeTimes: [UInt64: UInt64] = [:]
    private var roundTrips: [Double] = []
    private var response: AOOProbeResponse?
    private var didFinish = false

    init(
        endpoint: NWEndpoint,
        interface: NWInterface?,
        ipVersion: AOOProbeIPVersion,
        queue: DispatchQueue,
        completion: @escaping Completion
    ) {
        self.endpoint = endpoint
        self.interface = interface
        self.ipVersion = ipVersion
        self.queue = queue
        self.completion = completion
    }

    func start() {
        let parameters = NWParameters.udp
        parameters.prohibitExpensivePaths = true
        if let ipOptions = parameters.defaultProtocolStack.internetProtocol
            as? NWProtocolIP.Options {
            ipOptions.version = ipVersion.networkVersion
        }
        if let interface {
            parameters.requiredInterface = interface
        }
        let connection = NWConnection(to: endpoint, using: parameters)
        self.connection = connection
        connection.stateUpdateHandler = { [weak self] state in
            guard let self else { return }
            switch state {
            case .ready:
                self.beginProbes()
            case .failed, .cancelled:
                self.finish()
            default:
                break
            }
        }
        connection.start(queue: queue)
        queue.asyncAfter(deadline: .now() + 1.5) { [weak self] in
            self?.finish()
        }
    }

    func cancel() {
        guard !didFinish else { return }
        didFinish = true
        connection?.cancel()
        connection = nil
    }

    private func beginProbes() {
        receiveNext()
        for index in 0..<AOODiscoveryConstants.probeCount {
            queue.asyncAfter(deadline: .now() + .milliseconds(index * 35)) { [weak self] in
                self?.sendProbe()
            }
        }
        queue.asyncAfter(deadline: .now() + .milliseconds(750)) { [weak self] in
            self?.finish()
        }
    }

    private func sendProbe() {
        guard !didFinish, let connection else { return }
        let nonce = UInt64.random(in: UInt64.min...UInt64.max)
        let request = AOOProbeRequest(
            version: AOODiscoveryConstants.protocolVersion,
            nonce: nonce
        )
        guard let data = try? JSONEncoder().encode(request) else { return }
        pendingProbeTimes[nonce] = DispatchTime.now().uptimeNanoseconds
        connection.send(content: data, completion: .contentProcessed { _ in })
    }

    private func receiveNext() {
        guard !didFinish, let connection else { return }
        connection.receiveMessage { [weak self] data, _, _, error in
            guard let self, !self.didFinish else { return }
            if error != nil {
                self.finish()
                return
            }
            if let data,
               let response = try? JSONDecoder().decode(
                   AOOProbeResponse.self,
                   from: data
               ),
               response.version == AOODiscoveryConstants.protocolVersion,
               let start = self.pendingProbeTimes.removeValue(forKey: response.nonce) {
                let elapsed = DispatchTime.now().uptimeNanoseconds &- start
                self.roundTrips.append(Double(elapsed) / 1_000_000)
                self.response = response
            }
            self.receiveNext()
        }
    }

    private func finish() {
        guard !didFinish else { return }
        didFinish = true
        connection?.cancel()
        connection = nil

        guard let response,
              !response.receiverHost.isEmpty,
              roundTrips.count >= 3 else {
            completion(nil)
            return
        }
        let sorted = roundTrips.sorted()
        let median = Self.percentile(sorted, fraction: 0.5)
        let p95 = Self.percentile(sorted, fraction: 0.95)
        let p99 = Self.percentile(sorted, fraction: 0.99)
        let requestedCount = AOODiscoveryConstants.probeCount
        let loss = 1 - Double(sorted.count) / Double(requestedCount)
        let kind = Self.interfaceKind(
            interface: interface,
            receiverHost: response.receiverHost
        )
        completion(AOOPathCandidate(
            receiver: response.receiver,
            host: response.receiverHost,
            interfaceName: interface?.name ?? "",
            interfaceIndex: interface?.index ?? 0,
            interfaceKind: kind,
            medianRoundTripMilliseconds: median,
            p95RoundTripMilliseconds: p95,
            p99RoundTripMilliseconds: p99,
            packetLoss: loss,
            successfulProbeCount: sorted.count,
            requestedProbeCount: requestedCount
        ))
    }

    private static func percentile(_ sorted: [Double], fraction: Double) -> Double {
        guard !sorted.isEmpty else { return .infinity }
        let position = Int(ceil(fraction * Double(sorted.count))) - 1
        return sorted[min(max(position, 0), sorted.count - 1)]
    }

    private static func interfaceKind(
        interface: NWInterface?,
        receiverHost: String
    ) -> AOOInterfaceKind {
        if interface?.type == .wifi {
            return .wifi
        }
        if Self.isLinkLocal(receiverHost) {
            return .direct
        }
        if interface?.type == .wiredEthernet {
            return .wired
        }
        return .other
    }

    private static func isLinkLocal(_ host: String) -> Bool {
        let normalized = host.lowercased()
        return normalized.hasPrefix("169.254.")
            || normalized.hasPrefix("fe80:")
            || normalized.hasPrefix("[fe80:")
    }
}

private extension NSLock {
    func withLock<Result>(_ body: () throws -> Result) rethrows -> Result {
        lock()
        defer { unlock() }
        return try body()
    }
}
