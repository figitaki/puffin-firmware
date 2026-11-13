import Foundation
import CryptoKit

enum UpdateProtocol {
    static let service = "B9B75000-892A-4B9E-9E34-9F1B8DC7B56A"
    static let control = "B9B75001-892A-4B9E-9E34-9F1B8DC7B56A"
    static let data = "B9B75002-892A-4B9E-9E34-9F1B8DC7B56A"
    static let status = "B9B75003-892A-4B9E-9E34-9F1B8DC7B56A"

    static func uint32(_ value: UInt32) -> Data {
        Data((0..<4).map { UInt8(truncatingIfNeeded: value >> ($0 * 8)) })
    }

    static func authenticate(_ pin: String) throws -> Data {
        let bytes = Array(pin.utf8)
        guard bytes.count == 6, bytes.allSatisfy({ (48...57).contains($0) }) else {
            throw UpdateFailure("Enter exactly six ASCII digits from Puffin’s display.")
        }
        return Data([1] + bytes)
    }

    static let enableStreaming = Data([6])
    static func begin(size: UInt32) -> Data { Data([2]) + uint32(size) }
    static func digest(_ digest: Data, half: Int) -> Data {
        precondition(digest.count == 32 && (0...1).contains(half))
        return Data([3, UInt8(half)]) + digest.subdata(in: half * 16..<(half + 1) * 16)
    }
    static func packet(offset: UInt32, bytes: Data) -> Data { uint32(offset) + bytes }
}

struct UpdateFailure: LocalizedError {
    let message: String
    init(_ message: String) { self.message = message }
    var errorDescription: String? { message }
}

struct BoardStatus: Equatable {
    enum State: UInt8 { case ready, authorized, receiving, complete, error }
    enum BoardError: UInt8 {
        case none, authentication, locked, protocolViolation, size, offset, flash, verification, timeout
        var message: String {
            switch self {
            case .none: return "No board error."
            case .authentication: return "Incorrect PIN. Reset into maintenance mode and reconnect. Five bad attempts lock the session."
            case .locked: return "PIN attempts exhausted. Reset Puffin to open a new maintenance session."
            case .protocolViolation: return "Puffin rejected the update protocol. Check firmware compatibility."
            case .size: return "The image does not fit Puffin’s inactive OTA slot."
            case .offset: return "Puffin rejected a packet offset. The transfer was discarded."
            case .flash: return "Puffin could not write flash. USB recovery may be needed."
            case .verification: return "Puffin rejected the digest or ESP image. Use a trusted ProS3 application image."
            case .timeout: return "Puffin’s maintenance window or transfer timed out. Reset into maintenance mode."
            }
        }
    }
    let state: State
    let error: BoardError
    let written: UInt32
    let total: UInt32

    init(data: Data) throws {
        let b = Array(data)
        guard b.count == 12, b[0] == 1, b[3] == 0,
              let state = State(rawValue: b[1]), let error = BoardError(rawValue: b[2]) else {
            throw UpdateFailure("Invalid or unsupported Puffin status packet.")
        }
        func number(_ start: Int) -> UInt32 {
            (0..<4).reduce(UInt32(0)) { $0 | UInt32(b[start + $1]) << ($1 * 8) }
        }
        let written = number(4), total = number(8)
        guard written <= total,
              (state == .error ? error != .none : error == .none),
              ![.ready, .authorized].contains(state) || (written == 0 && total == 0),
              state != .receiving || total > 0,
              state != .complete || (total > 0 && written == total) else {
            throw UpdateFailure("Inconsistent Puffin status. Disconnect and reset the board.")
        }
        self.state = state; self.error = error; self.written = written; self.total = total
    }
}

/// Each write is retired exactly once, only after its ATT acknowledgment AND its
/// expected status (when required). Notifications may precede acknowledgments.
struct WriteGate {
    let expectedState: BoardStatus.State?
    let written: UInt32
    let total: UInt32
    private(set) var acknowledged = false
    private(set) var observed = false
    private(set) var retired = false

    init(state: BoardStatus.State? = nil, written: UInt32 = 0, total: UInt32 = 0) {
        expectedState = state; self.written = written; self.total = total
    }
    mutating func acknowledge() { acknowledged = true }
    mutating func observe(_ status: BoardStatus) {
        if status.state == expectedState && status.error == .none && status.written == written && status.total == total {
            observed = true
        }
    }
    mutating func retireIfReady() -> Bool {
        guard !retired, acknowledged, expectedState == nil || observed else { return false }
        retired = true
        return true
    }
}

/// The production streaming cursor. Queued bytes are not committed bytes: only
/// an exact cumulative window acknowledgment opens the next window (or Finish).
struct TransferWindow {
    static let packetLimit = 8
    let total: UInt32
    let payloadCapacity: Int
    private(set) var sent: UInt32 = 0
    private(set) var acknowledged: UInt32 = 0
    private(set) var packetsInWindow = 0

    init(total: UInt32, maximumWriteLength: Int) throws {
        guard total > 0, maximumWriteLength > 4 else {
            throw UpdateFailure("Bluetooth streaming write capacity is too small.")
        }
        self.total = total
        payloadCapacity = min(512, maximumWriteLength) - 4
    }

    var awaitingAcknowledgment: Bool { packetsInWindow == Self.packetLimit || sent == total }
    var canFinish: Bool { acknowledged == total }

    mutating func nextPacket(canSend: Bool) -> Range<Int>? {
        guard canSend, !awaitingAcknowledgment else { return nil }
        let start = Int(sent)
        let end = min(start + payloadCapacity, Int(total))
        sent = UInt32(end)
        packetsInWindow += 1
        return start..<end
    }

    /// Duplicate/stale committed progress is harmless. New progress must be the
    /// exact end of a fully queued window, never an intermediate or future offset.
    mutating func observe(_ status: BoardStatus) throws -> Bool {
        guard status.state == .receiving, status.error == .none, status.total == total else {
            throw UpdateFailure("Unexpected streaming status from Puffin. Transfer stopped.")
        }
        if status.written <= acknowledged { return false }
        guard awaitingAcknowledgment, status.written == sent else {
            throw UpdateFailure("Unexpected streaming offset from Puffin. Transfer stopped.")
        }
        acknowledged = sent
        packetsInWindow = 0
        return true
    }
}

struct FirmwareImage {
    // A client safety bound, NOT a claim about the board’s actual OTA capacity.
    // Begin is still subject to the inactive partition’s smaller device-side limit.
    static let maximumSize = 8 * 1024 * 1024
    let name: String
    let bytes: Data
    let digest: Data
    var sha256: String { digest.map { String(format: "%02x", $0) }.joined() }

    init(name: String, bytes: Data) throws {
        guard name.lowercased().hasSuffix(".bin") else { throw UpdateFailure("Select an application firmware.bin file.") }
        guard (33...Self.maximumSize).contains(bytes.count) else {
            throw UpdateFailure("Application image must be between 33 bytes and 8 MiB. The board may have a smaller OTA slot.")
        }
        let b = Array(bytes.prefix(24))
        guard b[0] == 0xE9, (1...16).contains(b[1]), b[2] <= 3,
              b[12] == 9, b[13] == 0, b[23] <= 1 else {
            throw UpdateFailure("Not a plausible ESP32-S3 application image. Do not select a merged whole-flash image, bootloader or partition table.")
        }
        var cursor = 24
        for _ in 0..<Int(b[1]) {
            guard cursor + 8 <= bytes.count else { throw UpdateFailure("Truncated ESP segment header.") }
            let length = (0..<4).reduce(UInt32(0)) { $0 | UInt32(bytes[cursor + 4 + $1]) << ($1 * 8) }
            cursor += 8
            guard length > 0, UInt64(length) <= UInt64(bytes.count - cursor) else {
                throw UpdateFailure("Truncated or empty ESP image segment.")
            }
            cursor += Int(length)
        }
        guard cursor < bytes.count, b[23] == 0 || bytes.count - cursor >= 33 else {
            throw UpdateFailure("ESP image checksum or appended hash is missing.")
        }
        self.name = name; self.bytes = bytes; self.digest = Data(SHA256.hash(data: bytes))
    }

    static func load(_ url: URL, name: String? = nil) throws -> FirmwareImage {
        let scoped = url.startAccessingSecurityScopedResource()
        defer { if scoped { url.stopAccessingSecurityScopedResource() } }
        let handle = try FileHandle(forReadingFrom: url)
        defer { try? handle.close() }
        // Bounded even when a provider reports incorrect/missing metadata.
        var bytes = Data()
        while bytes.count <= maximumSize {
            let chunk = try handle.read(upToCount: min(64 * 1024, maximumSize + 1 - bytes.count)) ?? Data()
            if chunk.isEmpty { break }
            bytes.append(chunk)
        }
        return try FirmwareImage(name: name ?? url.lastPathComponent, bytes: bytes)
    }
}
