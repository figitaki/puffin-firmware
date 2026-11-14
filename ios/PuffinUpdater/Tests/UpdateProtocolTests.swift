import XCTest
import CryptoKit
@testable import PuffinUpdater

final class UpdateProtocolTests: XCTestCase {
    private func status(_ state: UInt8 = 2, error: UInt8 = 0, written: UInt32 = 176, total: UInt32 = 352) -> Data {
        Data([1, state, error, 0]) + UpdateProtocol.uint32(written) + UpdateProtocol.uint32(total)
    }

    private func plausibleImage(size: Int = 64) -> Data {
        var bytes = Data(repeating: 0, count: size)
        bytes[0] = 0xE9; bytes[1] = 1; bytes[12] = 9
        bytes.replaceSubrange(28..<32, with: UpdateProtocol.uint32(UInt32(size - 33)))
        return bytes
    }

    func testStatusLittleEndianAndAllStates() throws {
        let parsed = try BoardStatus(data: status(written: 0x123456, total: 0x234567))
        XCTAssertEqual(parsed.written, 0x123456)
        XCTAssertEqual(parsed.total, 0x234567)
        for state in [UInt8(0), 1] {
            XCTAssertNoThrow(try BoardStatus(data: status(state, written: 0, total: 0)))
        }
        XCTAssertNoThrow(try BoardStatus(data: status(3, written: 352)))
        for error in UInt8(1)...UInt8(8) {
            XCTAssertNoThrow(try BoardStatus(data: status(4, error: error)))
        }
    }

    func testMalformedAndInconsistentStatusFailsClosed() {
        for length in [0, 1, 11, 13, 24] {
            XCTAssertThrowsError(try BoardStatus(data: Data(repeating: 0, count: length)))
        }
        let invalidFields: [(Int, UInt8)] = [(0, 2), (1, 5), (2, 9), (3, 1)]
        for (index, value) in invalidFields {
            var malformed = status(); malformed[index] = value
            XCTAssertThrowsError(try BoardStatus(data: malformed))
        }
        XCTAssertThrowsError(try BoardStatus(data: status(written: 353)))
        XCTAssertThrowsError(try BoardStatus(data: status(0)))
        XCTAssertThrowsError(try BoardStatus(data: status(1)))
        XCTAssertThrowsError(try BoardStatus(data: status(3)))
        XCTAssertThrowsError(try BoardStatus(data: status(4)))
        XCTAssertThrowsError(try BoardStatus(data: status(error: 1)))
        XCTAssertThrowsError(try BoardStatus(data: status(written: 0, total: 0)))
        XCTAssertThrowsError(try BoardStatus(data: status(3, written: 0, total: 0)))
    }

    func testCommandsUseExactWireFormat() throws {
        XCTAssertEqual(try UpdateProtocol.authenticate("012345"), Data([1, 48, 49, 50, 51, 52, 53]))
        for pin in ["", "12345", "1234567", "１２３４５６", "12345a", "123 45"] {
            XCTAssertThrowsError(try UpdateProtocol.authenticate(pin))
        }
        XCTAssertEqual(UpdateProtocol.begin(size: 0x12345678), Data([2, 0x78, 0x56, 0x34, 0x12]))
        XCTAssertEqual(UpdateProtocol.enableStreaming, Data([6]))
        let digest = Data(0..<32)
        XCTAssertEqual(UpdateProtocol.digest(digest, half: 0), Data([3, 0]) + Data(0..<16))
        XCTAssertEqual(UpdateProtocol.digest(digest, half: 1), Data([3, 1]) + Data(16..<32))
        XCTAssertEqual(UpdateProtocol.packet(offset: 256, bytes: Data([7])), Data([0, 1, 0, 0, 7]))
    }

    func testGateHandlesBothEventOrdersAndRetiresOnlyOnce() throws {
        let progress = try BoardStatus(data: status())
        for statusFirst in [true, false] {
            var gate = WriteGate(state: .receiving, written: 176, total: 352)
            if statusFirst { gate.observe(progress) } else { gate.acknowledge() }
            XCTAssertFalse(gate.retireIfReady())
            if statusFirst { gate.acknowledge() } else { gate.observe(progress) }
            XCTAssertTrue(gate.retireIfReady())
            gate.observe(progress); gate.acknowledge()
            XCTAssertFalse(gate.retireIfReady())
        }
    }

    func testStaleDuplicateOrWrongStatusCannotAdvanceNextPacket() throws {
        var gate = WriteGate(state: .receiving, written: 352, total: 352)
        gate.acknowledge()
        for bytes in [status(), status(), status(written: 352, total: 400), status(3, written: 352)] {
            gate.observe(try BoardStatus(data: bytes))
            XCTAssertFalse(gate.retireIfReady())
        }
        gate.observe(try BoardStatus(data: status(written: 352)))
        XCTAssertTrue(gate.retireIfReady())
    }

    func testFinishNeedsCompleteAndAckNotJustFinalProgress() throws {
        var gate = WriteGate(state: .complete, written: 352, total: 352)
        gate.observe(try BoardStatus(data: status(written: 352)))
        gate.acknowledge()
        XCTAssertFalse(gate.retireIfReady())
        gate.observe(try BoardStatus(data: status(3, written: 352)))
        XCTAssertTrue(gate.retireIfReady())
        var digestGate = WriteGate()
        XCTAssertFalse(digestGate.retireIfReady())
        digestGate.acknowledge()
        XCTAssertTrue(digestGate.retireIfReady())
    }

    func testStreamingNegotiatedPayloadCapAndSmallMTU() throws {
        for (maximum, payload) in [(1024, 508), (512, 508), (180, 176), (20, 16), (5, 1)] {
            var window = try TransferWindow(total: 2000, maximumWriteLength: maximum)
            XCTAssertEqual(window.payloadCapacity, payload)
            let range = try XCTUnwrap(window.nextPacket(canSend: true))
            let packet = UpdateProtocol.packet(offset: UInt32(range.lowerBound), bytes: Data(repeating: 7, count: range.count))
            XCTAssertEqual(packet.count, payload + 4)
            XCTAssertLessThanOrEqual(packet.count, min(512, maximum))
        }
        for maximum in [-1, 0, 4] {
            XCTAssertThrowsError(try TransferWindow(total: 100, maximumWriteLength: maximum))
        }
        XCTAssertThrowsError(try TransferWindow(total: 0, maximumWriteLength: 512))
    }

    func testStreamingEightPacketBoundAndBackpressureResume() throws {
        var window = try TransferWindow(total: 3520, maximumWriteLength: 180)
        XCTAssertNil(window.nextPacket(canSend: false))
        XCTAssertEqual(window.sent, 0)
        for packet in 0..<8 {
            XCTAssertEqual(window.nextPacket(canSend: true), packet * 176..<(packet + 1) * 176)
            XCTAssertNil(window.nextPacket(canSend: false))
            XCTAssertEqual(window.packetsInWindow, packet + 1)
        }
        XCTAssertTrue(window.awaitingAcknowledgment)
        XCTAssertNil(window.nextPacket(canSend: true))
        XCTAssertEqual(window.acknowledged, 0)
        XCTAssertTrue(try window.observe(BoardStatus(data: status(written: 1408, total: 3520))))
        XCTAssertNil(window.nextPacket(canSend: false))
        XCTAssertEqual(window.nextPacket(canSend: true), 1408..<1584)
    }

    func testStreamingStaleInvalidAndOutOfRangeAcknowledgments() throws {
        var window = try TransferWindow(total: 3520, maximumWriteLength: 180)
        _ = window.nextPacket(canSend: true)
        // Even an exact sent offset cannot acknowledge a partial (nonfinal) window.
        XCTAssertThrowsError(try window.observe(BoardStatus(data: status(total: 3520))))
        for _ in 1..<8 { _ = window.nextPacket(canSend: true) }
        for bytes in [status(written: 1407, total: 3520), status(written: 1409, total: 3520),
                      status(written: 3520, total: 3520), status(written: 1408, total: 4000),
                      status(3, written: 3520, total: 3520), status(1, written: 0, total: 0)] {
            XCTAssertThrowsError(try window.observe(BoardStatus(data: bytes)))
            XCTAssertEqual(window.acknowledged, 0)
        }
        XCTAssertFalse(try window.observe(BoardStatus(data: status(written: 0, total: 3520))))
        XCTAssertTrue(try window.observe(BoardStatus(data: status(written: 1408, total: 3520))))
        _ = window.nextPacket(canSend: true)
        for offset in [UInt32(0), 176, 1408, 1408] {
            XCTAssertFalse(try window.observe(BoardStatus(data: status(written: offset, total: 3520))))
            XCTAssertEqual(window.sent, 1584)
            XCTAssertEqual(window.packetsInWindow, 1)
        }
        XCTAssertThrowsError(try BoardStatus(data: status(written: 3521, total: 3520)))
    }

    func testStreamingFinalAcknowledgmentBeforeFinishAndCompleteGate() throws {
        // Short first/final window, exact eight packets, and a shortened ninth.
        for size in [UInt32(1), 176, 1408, 1409] {
            var window = try TransferWindow(total: size, maximumWriteLength: 180)
            while !window.canFinish {
                while window.nextPacket(canSend: true) != nil {}
                XCTAssertFalse(window.canFinish)
                XCTAssertNil(window.nextPacket(canSend: true))
                let end = window.sent
                XCTAssertTrue(try window.observe(BoardStatus(data: status(written: end, total: size))))
            }
            XCTAssertEqual(window.acknowledged, size)
            XCTAssertNil(window.nextPacket(canSend: true))
            XCTAssertFalse(try window.observe(BoardStatus(data: status(written: size, total: size))))
            for completeFirst in [true, false] {
                var finish = WriteGate(state: .complete, written: size, total: size)
                finish.observe(try BoardStatus(data: status(written: size, total: size)))
                XCTAssertFalse(finish.retireIfReady())
                if completeFirst { finish.observe(try BoardStatus(data: status(3, written: size, total: size))) }
                else { finish.acknowledge() }
                XCTAssertFalse(finish.retireIfReady())
                if completeFirst { finish.acknowledge() }
                else { finish.observe(try BoardStatus(data: status(3, written: size, total: size))) }
                XCTAssertTrue(finish.retireIfReady())
                XCTAssertFalse(finish.retireIfReady())
            }
        }
    }

    func testPlausibleImageAndSHA256() throws {
        let bytes = plausibleImage()
        let image = try FirmwareImage(name: "firmware.bin", bytes: bytes)
        XCTAssertEqual(image.digest, Data(SHA256.hash(data: bytes)))
        XCTAssertEqual(image.sha256.count, 64)
        XCTAssertEqual(image.bytes, bytes)
    }

    func testImageBoundsAndBadHeaders() {
        for size in [0, 24, 32, FirmwareImage.maximumSize + 1] {
            XCTAssertThrowsError(try FirmwareImage(name: "firmware.bin", bytes: Data(repeating: 0, count: size)))
        }
        XCTAssertNoThrow(try FirmwareImage(name: "firmware.bin", bytes: plausibleImage(size: FirmwareImage.maximumSize)))
        XCTAssertThrowsError(try FirmwareImage(name: "firmware.hex", bytes: plausibleImage()))
        let invalidFields: [(Int, UInt8)] = [(0, 0), (1, 0), (1, 17), (2, 4), (12, 0), (13, 1), (23, 2)]
        for (index, value) in invalidFields {
            var bytes = plausibleImage(); bytes[index] = value
            XCTAssertThrowsError(try FirmwareImage(name: "firmware.bin", bytes: bytes))
        }
    }

    func testFileLoaderRejectsOversizeAndReadsValidFile() throws {
        let directory = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: directory) }
        let url = directory.appendingPathComponent("firmware.bin")
        try plausibleImage().write(to: url)
        XCTAssertEqual(try FirmwareImage.load(url).bytes, plausibleImage())
        try Data(repeating: 0, count: FirmwareImage.maximumSize + 1).write(to: url)
        XCTAssertThrowsError(try FirmwareImage.load(url))
    }

    func testTruncatedSegmentsAndMissingChecksumOrHash() {
        var bytes = plausibleImage()
        bytes.replaceSubrange(28..<32, with: UpdateProtocol.uint32(UInt32.max))
        XCTAssertThrowsError(try FirmwareImage(name: "firmware.bin", bytes: bytes))
        bytes = plausibleImage(); bytes[1] = 2
        XCTAssertThrowsError(try FirmwareImage(name: "firmware.bin", bytes: bytes))
        bytes = plausibleImage(); bytes[28] = 0
        XCTAssertThrowsError(try FirmwareImage(name: "firmware.bin", bytes: bytes))
        bytes = plausibleImage(); bytes.removeLast()
        XCTAssertThrowsError(try FirmwareImage(name: "firmware.bin", bytes: bytes))
        bytes = plausibleImage(); bytes[23] = 1
        XCTAssertThrowsError(try FirmwareImage(name: "firmware.bin", bytes: bytes))
    }

    func testOfficialReleaseRequiresCompatibleHTTPSManifest() throws {
        let valid: [String: Any] = ["version": "1.2.3", "notes": ["Improved sequencing"],
            "target": "ProS3", "url": "https://updates.example.invalid/puffin.bin",
            "size": 64, "sha256": String(repeating: "a", count: 64)]
        let parsed = try FirmwareRelease.decode(JSONSerialization.data(withJSONObject: valid))
        XCTAssertEqual(parsed.version, "1.2.3")
        XCTAssertEqual(parsed.notes, ["Improved sequencing"])
        for (key, value) in [("url", "http://updates.example.invalid/puffin.bin"),
                             ("url", "file:///tmp/puffin.bin"),
                             ("url", "https://user:password@example.invalid/puffin.bin"),
                             ("target", "ESP32"), ("version", ""), ("sha256", String(repeating: "z", count: 64))] {
            var invalid = valid; invalid[key] = value
            XCTAssertThrowsError(try FirmwareRelease.decode(JSONSerialization.data(withJSONObject: invalid)))
        }
        for size in [32, FirmwareImage.maximumSize + 1] {
            var invalid = valid; invalid["size"] = size
            XCTAssertThrowsError(try FirmwareRelease.decode(JSONSerialization.data(withJSONObject: invalid)))
        }
        XCTAssertThrowsError(try FirmwareRelease.decode(Data(repeating: 0, count: 65537)))
    }

    func testOfficialReleaseBindsExactDownloadedImage() throws {
        let image = try FirmwareImage(name: "firmware.bin", bytes: plausibleImage())
        let valid = FirmwareRelease(version: "1.2.3", notes: [], target: "ProS3",
            url: URL(string: "https://example.invalid/firmware.bin")!, size: image.bytes.count, sha256: image.sha256)
        XCTAssertNoThrow(try valid.validate(image))
        var tampered = image.bytes; tampered[40] ^= 1
        XCTAssertThrowsError(try valid.validate(FirmwareImage(name: "firmware.bin", bytes: tampered)))
        let wrongSize = FirmwareRelease(version: valid.version, notes: [], target: "ProS3",
            url: valid.url, size: valid.size + 1, sha256: image.sha256)
        XCTAssertThrowsError(try wrongSize.validate(image))
        for (url, code) in [("https://example.invalid/firmware.bin", 200),
                            ("https://example.invalid/firmware.bin", 404),
                            ("http://example.invalid/firmware.bin", 200)] {
            let response = HTTPURLResponse(url: URL(string: url)!, statusCode: code, httpVersion: nil, headerFields: nil)!
            if code == 200 && url.hasPrefix("https:") { XCTAssertNoThrow(try FirmwareRelease.checkResponse(response)) }
            else { XCTAssertThrowsError(try FirmwareRelease.checkResponse(response)) }
        }
    }
}
