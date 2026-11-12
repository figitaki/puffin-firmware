import Foundation
import CoreBluetooth
import Combine

@MainActor
final class Updater: NSObject, ObservableObject, CBCentralManagerDelegate, CBPeripheralDelegate {
    enum Phase { case idle, importing, scanning, connecting, preparing, pin, transferring, complete, failed }
    enum FailureStage { case service, connection, transfer }
    struct Device: Identifiable { let id: UUID; let name: String }
    private enum Operation { case authenticate, digest0, digest1, begin, packet(UInt32), finish }
    private struct Pending {
        let operation: Operation
        let characteristic: CBUUID
        var gate: WriteGate
    }

    @Published private(set) var phase: Phase = .idle
    @Published private(set) var message = "Choose trusted firmware to begin."
    @Published private(set) var image: FirmwareImage?
    @Published private(set) var release: FirmwareRelease?
    @Published private(set) var devices: [Device] = []
    @Published private(set) var written: UInt32 = 0
    @Published private(set) var disconnecting = false
    @Published private(set) var failureStage: FailureStage = .service
    let preview: Bool
    private var central: CBCentralManager?
    private var discovered: [UUID: CBPeripheral] = [:]
    private var peripheral: CBPeripheral?
    private var control: CBCharacteristic?
    private var data: CBCharacteristic?
    private var status: CBCharacteristic?
    private var pending: Pending?
    private var waitingForInitialStatus = false
    private var deadline: Task<Void, Never>?
    private var importID = UUID()
    private var downloadTask: Task<Void, Never>?

    var canStart: Bool { !preview && !disconnecting && [.idle, .failed, .complete].contains(phase) }
    var active: Bool { [.scanning, .connecting, .preparing, .pin, .transferring, .importing].contains(phase) }
    var total: UInt32 { UInt32(image?.bytes.count ?? 0) }

    init(preview: String? = nil) {
        self.preview = preview != nil
        super.init()
        if let preview { configurePreview(preview) }
    }

    func checkForUpdates() {
        guard canStart else { return }
        failureStage = .service
        guard let value = Bundle.main.object(forInfoDictionaryKey: "PuffinReleaseManifestURL") as? String,
              let url = URL(string: value), url.scheme == "https", url.host != nil,
              url.user == nil, url.password == nil else {
            fail("The official update service hasn’t been configured for this build."); return
        }
        let token = UUID(); importID = token
        image = nil; release = nil; phase = .importing; message = "Checking for updates…"
        downloadTask = Task {
            do {
                var request = URLRequest(url: url, cachePolicy: .reloadIgnoringLocalCacheData, timeoutInterval: 30)
                request.setValue("application/json", forHTTPHeaderField: "Accept")
                let (bytes, response) = try await URLSession.shared.data(for: request)
                try FirmwareRelease.checkResponse(response)
                let available = try FirmwareRelease.decode(bytes)
                guard importID == token, !Task.isCancelled else { return }
                release = available; phase = .idle; message = "An update is available."
            } catch {
                guard importID == token, !Task.isCancelled else { return }
                fail(error.localizedDescription)
            }
        }
    }

    func downloadUpdate() {
        guard canStart, let release else { return }
        failureStage = .service
        let token = UUID(); importID = token
        image = nil; phase = .importing; message = "Downloading your update…"
        downloadTask = Task {
            do {
                let request = URLRequest(url: release.url, timeoutInterval: 120)
                let (file, response) = try await URLSession.shared.download(for: request)
                defer { try? FileManager.default.removeItem(at: file) }
                try FirmwareRelease.checkResponse(response)
                let loaded = try FirmwareImage.load(file, name: "firmware.bin")
                try release.validate(loaded)
                guard importID == token, !Task.isCancelled else { return }
                image = loaded; phase = .idle
                scan()
            } catch {
                guard importID == token, !Task.isCancelled else { return }
                fail(error.localizedDescription)
            }
        }
    }

    func importFile(_ url: URL) {
        guard canStart else { return }
        failureStage = .service
        let token = UUID(); importID = token
        phase = .importing; image = nil; release = nil; written = 0; message = "Preparing your update…"
        Task {
            do {
                let loaded = try await Task.detached(priority: .userInitiated) { try FirmwareImage.load(url) }.value
                guard importID == token else { return }
                image = loaded; phase = .idle; message = "Image checked locally. Ready to find Puffin."
            } catch {
                guard importID == token else { return }
                fail(error.localizedDescription)
            }
        }
    }

    func reportImportError(_ error: Error) { if canStart { fail("Could not open file: \(error.localizedDescription)") } }

    func scan() {
        guard canStart, image != nil else { return }
        failureStage = .connection
        written = 0; devices = []; discovered = [:]
        phase = .scanning; message = "Looking for Puffin in maintenance mode…"
        armTimeout("No Puffin selected in time. Hold Fn+Up while resetting the board, then scan again.", seconds: 30)
        if central == nil {
            central = CBCentralManager(delegate: self, queue: .main)
        } else { startScanIfPoweredOn() }
    }

    private func startScanIfPoweredOn() {
        guard let central, phase == .scanning else { return }
        switch central.state {
        case .poweredOn:
            central.scanForPeripherals(withServices: [CBUUID(string: UpdateProtocol.service)], options: nil)
        case .unknown, .resetting: break
        case .unauthorized: fail("Bluetooth access denied. Allow Puffin Updater in Settings → Privacy & Security → Bluetooth.")
        case .poweredOff: fail("Bluetooth is off. Turn it on in Settings, then scan again.")
        case .unsupported: fail("Bluetooth LE is unavailable on this device. Use a physical iPhone or iPad.")
        @unknown default: fail("Bluetooth is unavailable.")
        }
    }

    func connect(_ id: UUID) {
        guard !preview, phase == .scanning, let device = discovered[id] else { return }
        central?.stopScan(); peripheral = device; device.delegate = self
        phase = .connecting; message = "Connecting to Puffin…"
        armTimeout("Connection timed out. Reset Puffin into maintenance mode and try again.")
        central?.connect(device, options: nil)
    }

    func authenticate(pin: String) {
        guard !preview, phase == .pin, image != nil, let control else { return }
        failureStage = .transfer
        do {
            let command = try UpdateProtocol.authenticate(pin)
            phase = .transferring; message = "Checking PIN…"
            send(command, to: control, operation: .authenticate, gate: WriteGate(state: .authorized))
        } catch { message = error.localizedDescription }
    }

    func cancel() {
        guard !preview else { return }
        importID = UUID()
        downloadTask?.cancel(); downloadTask = nil
        let completed = phase == .complete
        releaseConnection()
        phase = .idle; written = 0
        if completed {
            image = nil; release = nil; message = "Ready for your next update."
        } else {
            message = "Canceled. Unfinished transfers are discarded. If verification had started, installation may already have completed; inspect Puffin before retrying from zero."
        }
    }

    func enteredBackground() {
        guard !preview, active else { return }
        importID = UUID()
        downloadTask?.cancel(); downloadTask = nil
        fail("Update stopped because the app left the foreground. Keep Puffin Updater open; reconnect and restart from zero.")
    }

    private func armTimeout(_ reason: String, seconds: UInt64 = 15) {
        deadline?.cancel()
        deadline = Task { [weak self] in
            do { try await Task.sleep(nanoseconds: seconds * 1_000_000_000) } catch { return }
            self?.fail(reason)
        }
    }

    private func fail(_ reason: String) {
        releaseConnection(); phase = .failed; message = reason
    }

    private func releaseConnection() {
        deadline?.cancel(); deadline = nil; central?.stopScan()
        pending = nil; waitingForInitialStatus = false
        control = nil; data = nil; status = nil
        if let peripheral {
            peripheral.delegate = nil
            if peripheral.state != .disconnected {
                disconnecting = central?.state == .poweredOn
                central?.cancelPeripheralConnection(peripheral)
            }
        }
        peripheral = nil; discovered = [:]; devices = []
    }

    private func send(_ bytes: Data, to characteristic: CBCharacteristic, operation: Operation, gate: WriteGate) {
        guard pending == nil, let peripheral, peripheral.state == .connected,
              bytes.count <= peripheral.maximumWriteValueLength(for: .withResponse) else {
            fail("Cannot safely queue this Bluetooth write. Transfer stopped."); return
        }
        pending = Pending(operation: operation, characteristic: characteristic.uuid, gate: gate)
        armTimeout("No acknowledgment or matching progress from Puffin. Transfer stopped; reconnect and restart from zero.")
        peripheral.writeValue(bytes, for: characteristic, type: .withResponse)
    }

    private func advanceIfReady() {
        guard var current = pending, current.gate.retireIfReady() else { return }
        pending = nil; deadline?.cancel()
        guard let image, let control, let data, let peripheral else { fail("Update session was lost."); return }
        switch current.operation {
        case .authenticate:
            message = "Sending SHA-256 digest…"
            send(UpdateProtocol.digest(image.digest, half: 0), to: control, operation: .digest0, gate: WriteGate())
        case .digest0:
            send(UpdateProtocol.digest(image.digest, half: 1), to: control, operation: .digest1, gate: WriteGate())
        case .digest1:
            message = "Opening inactive firmware slot…"
            send(UpdateProtocol.begin(size: total), to: control, operation: .begin,
                 gate: WriteGate(state: .receiving, total: total))
        case .begin, .packet:
            if case .packet(let committed) = current.operation { written = committed }
            if written == total {
                message = "Puffin is verifying the image…"
                send(Data([4]), to: control, operation: .finish,
                     gate: WriteGate(state: .complete, written: total, total: total))
            } else {
                let capacity = min(180, peripheral.maximumWriteValueLength(for: .withResponse)) - 4
                guard capacity > 0 else { fail("Bluetooth write capacity is too small."); return }
                let end = min(Int(written) + capacity, image.bytes.count)
                let payload = image.bytes.subdata(in: Int(written)..<end)
                message = "Transferring to Puffin. Keep this app open and stay nearby."
                send(UpdateProtocol.packet(offset: written, bytes: payload), to: data, operation: .packet(UInt32(end)),
                     gate: WriteGate(state: .receiving, written: UInt32(end), total: total))
            }
        case .finish:
            phase = .complete; message = "Transferred and verified; restarting"
            // Complete is the only success signal. Do not send Abort after it.
            releaseConnection()
        }
    }

    func centralManagerDidUpdateState(_ central: CBCentralManager) {
        if central.state != .poweredOn {
            disconnecting = false
            if peripheral != nil {
                fail("Bluetooth became unavailable. Transfer stopped; restart from zero."); return
            }
        }
        startScanIfPoweredOn()
    }

    func centralManager(_ central: CBCentralManager, didDiscover peripheral: CBPeripheral,
                        advertisementData: [String: Any], rssi RSSI: NSNumber) {
        guard phase == .scanning, discovered[peripheral.identifier] == nil else { return }
        discovered[peripheral.identifier] = peripheral
        devices.append(Device(id: peripheral.identifier, name: peripheral.name ?? "Puffin Update"))
    }

    func centralManager(_ central: CBCentralManager, didConnect peripheral: CBPeripheral) {
        guard peripheral === self.peripheral, phase == .connecting else { return }
        phase = .preparing; message = "Checking updater service and subscribing to status…"
        armTimeout("Puffin’s updater service did not become ready.")
        peripheral.discoverServices([CBUUID(string: UpdateProtocol.service)])
    }

    func centralManager(_ central: CBCentralManager, didFailToConnect peripheral: CBPeripheral, error: Error?) {
        disconnecting = false
        guard peripheral === self.peripheral else { return }
        fail("Could not connect: \(error?.localizedDescription ?? "connection failed").")
    }

    func centralManager(_ central: CBCentralManager, didDisconnectPeripheral peripheral: CBPeripheral, error: Error?) {
        disconnecting = false
        guard peripheral === self.peripheral else { return }
        fail("Puffin disconnected before verified completion. Installation is not confirmed. Reset into maintenance mode and restart from zero.")
    }

    func peripheral(_ peripheral: CBPeripheral, didDiscoverServices error: Error?) {
        guard peripheral === self.peripheral else { return }
        guard error == nil, let service = peripheral.services?.first(where: { $0.uuid == CBUUID(string: UpdateProtocol.service) }) else {
            fail("Updater service missing or inaccessible."); return
        }
        peripheral.discoverCharacteristics([UpdateProtocol.control, UpdateProtocol.data, UpdateProtocol.status].map { CBUUID(string: $0) }, for: service)
    }

    func peripheral(_ peripheral: CBPeripheral, didDiscoverCharacteristicsFor service: CBService, error: Error?) {
        guard peripheral === self.peripheral else { return }
        control = service.characteristics?.first { $0.uuid == CBUUID(string: UpdateProtocol.control) }
        data = service.characteristics?.first { $0.uuid == CBUUID(string: UpdateProtocol.data) }
        status = service.characteristics?.first { $0.uuid == CBUUID(string: UpdateProtocol.status) }
        guard error == nil, let control, let data, let status,
              control.properties.contains(.write), data.properties.contains(.write),
              status.properties.contains(.read), status.properties.contains(.notify),
              peripheral.maximumWriteValueLength(for: .withResponse) >= 18 else {
            fail("Updater characteristics or required read/notify/write-with-response capabilities are missing."); return
        }
        peripheral.setNotifyValue(true, for: status)
    }

    func peripheral(_ peripheral: CBPeripheral, didUpdateNotificationStateFor characteristic: CBCharacteristic, error: Error?) {
        guard peripheral === self.peripheral, characteristic.uuid == status?.uuid else { return }
        guard error == nil, characteristic.isNotifying else { fail("Could not subscribe to Puffin status."); return }
        waitingForInitialStatus = true
        peripheral.readValue(for: characteristic)
    }

    func peripheral(_ peripheral: CBPeripheral, didUpdateValueFor characteristic: CBCharacteristic, error: Error?) {
        guard peripheral === self.peripheral, characteristic.uuid == status?.uuid else { return }
        do {
            guard error == nil, let value = characteristic.value else { throw UpdateFailure("Could not read Puffin status.") }
            let board = try BoardStatus(data: value)
            if board.state == .error { fail(board.error.message); return }
            if waitingForInitialStatus {
                guard board.state == .ready else { fail("Puffin is not Ready. Reset into maintenance mode before reconnecting."); return }
                waitingForInitialStatus = false; deadline?.cancel()
                phase = .pin; message = "Connected. Enter the six-digit PIN on Puffin’s display."
                armTimeout("PIN entry timed out. Reset into maintenance mode and reconnect.", seconds: 120)
                return
            }
            // Ignore notifications before subscription/read setup completes.
            guard phase == .transferring, var current = pending else { return }
            if board.state == .complete {
                guard case .finish = current.operation, board.total == total else {
                    fail("Unexpected completion status; installation is not confirmed."); return
                }
            }
            if board.state == .receiving {
                guard board.total == total, board.written <= current.gate.written else {
                    fail("Unexpected progress from Puffin. Transfer stopped."); return
                }
            }
            current.gate.observe(board); pending = current
            advanceIfReady()
        } catch { fail(error.localizedDescription) }
    }

    func peripheral(_ peripheral: CBPeripheral, didWriteValueFor characteristic: CBCharacteristic, error: Error?) {
        guard peripheral === self.peripheral, var current = pending else { return }
        guard error == nil, current.characteristic == characteristic.uuid else {
            fail("Bluetooth write failed: \(error?.localizedDescription ?? "unexpected acknowledgment")."); return
        }
        current.gate.acknowledge(); pending = current
        advanceIfReady()
    }

    private func configurePreview(_ state: String) {
        // Design-only sample. No central manager, downloads, imports or writes.
        let sample = """
        {"version":"1.0.0","notes":["More ways to shape every step.","Smoother connections. Same familiar Puffin."],"target":"ProS3","url":"https://example.invalid/firmware.bin","size":1048576,"sha256":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"}
        """
        release = try? FirmwareRelease.decode(Data(sample.utf8))
        switch state {
        case "connect": phase = .scanning; devices = [Device(id: UUID(), name: "Puffin")]
        case "pin": phase = .pin
        case "transfer": phase = .transferring; written = 393216; message = "Transferring to Puffin. Keep this app open and stay nearby."
        case "success": phase = .complete; written = 1048576; message = "Transferred and verified; restarting"
        case "error": phase = .failed; failureStage = .transfer; message = "Puffin disconnected before verified completion. Installation is not confirmed. Restart from zero."
        default: phase = .idle; message = "Choose trusted firmware to begin."
        }
    }

    func advancePreview() {
        guard preview else { return }
        switch phase {
        case .idle, .failed: configurePreview("connect")
        case .complete: configurePreview("idle")
        case .scanning: configurePreview("pin")
        case .pin: configurePreview("transfer")
        case .transferring: configurePreview("success")
        default: break
        }
    }
}
