import SwiftUI
import UniformTypeIdentifiers

@main
@MainActor
struct PuffinUpdaterApp: App {
    @StateObject private var updater: Updater

    init() {
        let args = ProcessInfo.processInfo.arguments
        let index = args.firstIndex(of: "--preview-updater")
        let preview = index.map { $0 + 1 < args.count ? args[$0 + 1] : "idle" }
        _updater = StateObject(wrappedValue: Updater(preview: preview))
    }

    var body: some Scene {
        WindowGroup { UpdaterView(updater: updater) }
    }
}

@MainActor
struct UpdaterView: View {
    @ObservedObject var updater: Updater
    @Environment(\.scenePhase) private var scenePhase
    @State private var importing = false
    @State private var help = false
    @State private var cancelConfirmation = false
    @State private var pin = ""
    @FocusState private var pinFocused: Bool

    private let ink = Color(red: 0.13, green: 0.16, blue: 0.15)
    private let paper = Color(red: 0.97, green: 0.96, blue: 0.93)
    private let accent = Color(red: 0.84, green: 0.29, blue: 0.12)
    private var total: UInt32 { updater.preview ? 1048576 : updater.total }
    private var progress: Double { Double(updater.written) / Double(max(1, total)) }
    private var busy: Bool { [.importing, .connecting, .preparing].contains(updater.phase) }
    private var finalizing: Bool { updater.phase == .transferring && total > 0 && updater.written == total }
    private var actionDisabled: Bool { updater.disconnecting || (updater.phase == .pin && (try? UpdateProtocol.authenticate(pin)) == nil) }

    var body: some View {
        NavigationStack {
            ScrollViewReader { reader in
            ScrollView {
                VStack(alignment: .leading, spacing: 24) {
                    if updater.preview {
                        Label("DESIGN PREVIEW · No device activity", systemImage: "eye")
                            .font(.caption.weight(.medium)).foregroundStyle(.secondary)
                            .accessibilityIdentifier("simulation-banner")
                    }

                    deviceIllustration
                        .padding(.top, 14).accessibilityHidden(true)

                    VStack(alignment: .leading, spacing: 12) {
                        Text(eyebrow).font(.caption.weight(.bold)).tracking(2).foregroundStyle(accent)
                        Text(title).font(.system(.largeTitle, design: .rounded, weight: .bold))
                            .fixedSize(horizontal: false, vertical: true)
                            .accessibilityIdentifier("update-title")
                        Text(subtitle).font(.body).foregroundStyle(ink.opacity(0.65))
                            .fixedSize(horizontal: false, vertical: true)
                    }

                    content

                    if busy {
                        HStack(spacing: 12) {
                            ProgressView().tint(ink)
                            Text(updater.message).font(.subheadline)
                        }.frame(maxWidth: .infinity, alignment: .leading)
                    }

                    if updater.phase != .scanning && updater.phase != .transferring && !busy {
                        Button(action: primaryAction) {
                            HStack {
                                Text(buttonTitle)
                                Spacer()
                                Image(systemName: updater.phase == .complete ? "checkmark" : "arrow.right")
                            }.font(.headline).padding(20)
                                .frame(maxWidth: .infinity)
                                .contentShape(Rectangle())
                        }
                        .buttonStyle(.plain)
                        .background(actionDisabled ? ink.opacity(0.08) : ink, in: RoundedRectangle(cornerRadius: 18))
                        .foregroundStyle(actionDisabled ? ink.opacity(0.65) : Color.white)
                        .disabled(actionDisabled)
                        .accessibilityIdentifier("primary-action")
                    }

                    if updater.active && !updater.preview && !finalizing {
                        Button("Cancel update", role: .destructive) { cancelConfirmation = true }
                            .font(.subheadline).frame(maxWidth: .infinity)
                    }
                    if updater.preview && updater.phase == .transferring {
                        Button("Preview completion") { updater.advancePreview() }
                            .font(.subheadline).frame(maxWidth: .infinity)
                    }
                    if updater.disconnecting {
                        Text("Disconnecting from Puffin…").font(.footnote).foregroundStyle(.secondary)
                    }
                    HStack(spacing: 6) {
                        Image(systemName: "bolt.horizontal")
                        Text("A little update. More possibilities.")
                    }.font(.footnote).foregroundStyle(ink.opacity(0.45))
                        .frame(maxWidth: .infinity).padding(.bottom, 12)
                }
                .padding(28).frame(maxWidth: 520).frame(maxWidth: .infinity)
            }
            .onReceive(NotificationCenter.default.publisher(for: UIResponder.keyboardDidShowNotification)) { _ in
                if pinFocused {
                    withAnimation { reader.scrollTo("pin-entry", anchor: .top) }
                }
            }
            }
            .background(paper.ignoresSafeArea()).foregroundStyle(ink)
            .navigationTitle("puffin").navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .navigationBarTrailing) {
                    Menu {
                        Button { help = true } label: { Label("How to connect", systemImage: "questionmark.circle") }
                        Button { importing = true } label: { Label("Install local firmware…", systemImage: "doc") }
                            .disabled(!updater.canStart)
                    } label: {
                        Image(systemName: "ellipsis.circle").foregroundStyle(ink)
                    }.accessibilityLabel("Update options")
                }
            }
            .fileImporter(isPresented: $importing, allowedContentTypes: [.data]) { result in
                switch result {
                case .success(let url): updater.importFile(url)
                case .failure(let error): updater.reportImportError(error)
                }
            }
            .sheet(isPresented: $help) { connectionHelp }
            .confirmationDialog("Stop this update?", isPresented: $cancelConfirmation, titleVisibility: .visible) {
                Button("Stop and disconnect", role: .destructive) { pin = ""; updater.cancel() }
            } message: {
                Text("You’ll need to start again. If Puffin has begun installing, check its screen before trying another update.")
            }
            .onChange(of: scenePhase) { phase in
                if phase == .background { pin = ""; updater.enteredBackground() }
            }
            .onChange(of: updater.phase) { _ in pin = "" }
        }
        .tint(accent).preferredColorScheme(.light)
    }

    @ViewBuilder private var content: some View {
        switch updater.phase {
        case .idle:
            if let release = updater.release {
                VStack(alignment: .leading, spacing: 18) {
                    HStack {
                        Text("WHAT’S NEW").font(.caption.weight(.bold)).tracking(1.5)
                        Spacer()
                        Text("v\(release.version)").font(.caption.monospacedDigit())
                    }
                    ForEach(Array(release.notes.enumerated()), id: \.offset) { _, note in
                        HStack(alignment: .top, spacing: 12) {
                            Image(systemName: "plus").foregroundStyle(accent)
                            Text(note).font(.subheadline)
                        }
                    }
                }.padding(24).background(.white.opacity(0.8), in: RoundedRectangle(cornerRadius: 22))
            }
        case .scanning:
            VStack(spacing: 16) {
                ForEach(updater.devices) { device in
                    Button {
                        if updater.preview { updater.advancePreview() } else { updater.connect(device.id) }
                    } label: {
                        HStack(spacing: 14) {
                            Image(systemName: "pianokeys").font(.title2)
                            VStack(alignment: .leading, spacing: 4) {
                                Text(device.name).font(.headline)
                                Text("Nearby · \(device.id.uuidString.suffix(4))").font(.caption).foregroundStyle(.secondary)
                            }
                            Spacer()
                            Image(systemName: "arrow.right")
                        }.padding(22).background(.white, in: RoundedRectangle(cornerRadius: 20))
                            .contentShape(Rectangle())
                    }.buttonStyle(.plain).accessibilityIdentifier("connect-device")
                }
                if updater.devices.isEmpty {
                    HStack(spacing: 12) { ProgressView(); Text("Looking for your Puffin…").font(.subheadline) }
                        .frame(maxWidth: .infinity, alignment: .leading)
                }
                Button("Need help connecting?") { help = true }.font(.subheadline)
            }
        case .pin:
            TextField("Six-digit code", text: $pin, prompt: Text("000000").foregroundColor(ink.opacity(0.65)))
                .font(.system(size: 38, weight: .medium, design: .monospaced)).tracking(8)
                .keyboardType(.numberPad).textContentType(.oneTimeCode).focused($pinFocused)
                .multilineTextAlignment(.center).padding(22)
                .background(.white, in: RoundedRectangle(cornerRadius: 20))
                .accessibilityLabel("Six-digit code on Puffin").accessibilityIdentifier("board-pin")
                .id("pin-entry")
        case .transferring:
            VStack(alignment: .leading, spacing: 16) {
                HStack(alignment: .firstTextBaseline) {
                    Text("\(Int(progress * 100))").font(.system(size: 56, weight: .medium, design: .rounded))
                    Text("%").font(.title2).foregroundStyle(.secondary)
                    Spacer()
                    Text(finalizing ? "Verifying" : "Sending update").font(.subheadline)
                }
                ProgressView(value: progress).tint(accent)
                    .accessibilityLabel("Update progress").accessibilityValue("\(Int(progress * 100)) percent")
            }.padding(24).background(.white, in: RoundedRectangle(cornerRadius: 22))
        case .failed:
            DisclosureGroup("Details") {
                Text(updater.message).font(.footnote).foregroundStyle(.secondary)
                    .frame(maxWidth: .infinity, alignment: .leading).padding(.top, 8)
            }.font(.subheadline).padding(22).background(.white, in: RoundedRectangle(cornerRadius: 20))
        case .complete:
            Label("Transfer verified", systemImage: "checkmark.circle.fill")
                .font(.subheadline.weight(.medium)).foregroundStyle(Color(red: 0.25, green: 0.43, blue: 0.32))
        default: EmptyView()
        }
    }

    private var deviceIllustration: some View {
        ZStack {
            Circle().fill(Color.white.opacity(0.65)).frame(width: 160, height: 160)
            VStack(spacing: 10) {
                HStack {
                    Text("puffin").font(.system(size: 10, weight: .bold, design: .rounded)).foregroundStyle(.white.opacity(0.65))
                    Spacer()
                    RoundedRectangle(cornerRadius: 3).fill(accent).frame(width: 40, height: 11)
                }
                ForEach(0..<3) { row in
                    HStack(spacing: 5) {
                        ForEach(0..<10) { col in
                            RoundedRectangle(cornerRadius: 3)
                                .fill(row == 2 && col == 7 ? accent : Color.white.opacity(0.8))
                                .frame(height: 13)
                        }
                    }
                }
            }.padding(14).frame(width: 234).background(ink, in: RoundedRectangle(cornerRadius: 16))
                .rotationEffect(.degrees(-8))
                .shadow(color: ink.opacity(0.15), radius: 16, x: 0, y: 12)
            if updater.phase == .complete {
                Image(systemName: "checkmark").font(.title3.weight(.bold)).foregroundStyle(.white)
                    .frame(width: 44, height: 44).background(accent, in: Circle()).offset(x: 104, y: 46)
            }
        }.frame(maxWidth: .infinity).frame(height: 160)
    }

    private var connectionHelp: some View {
        NavigationStack {
            ScrollView {
                VStack(alignment: .leading, spacing: 24) {
                    Text("Let’s find your Puffin.").font(.largeTitle.bold())
                    Text("Keep Puffin powered and close to your phone.")
                    Label("Hold Fn + Up as you restart Puffin.", systemImage: "1.circle")
                    Label("Wait for a six-digit code on its screen.", systemImage: "2.circle")
                    Label("Choose Puffin here, then enter that code.", systemImage: "3.circle")
                    Text("Keep this app open while updating. To leave update mode, restart Puffin without holding any keys.")
                        .font(.subheadline).foregroundStyle(.secondary)
                    DisclosureGroup("Older firmware & recovery") {
                        Text("Older Puffin firmware needs a one-time USB update before wireless updates are available. If Puffin won’t restart after an update, use USB recovery.")
                            .font(.subheadline).padding(.top, 8)
                    }
                }.padding(28).frame(maxWidth: 520)
            }.navigationTitle("Connection help").navigationBarTitleDisplayMode(.inline)
                .toolbar { ToolbarItem(placement: .confirmationAction) { Button("Done") { help = false } } }
        }.tint(accent)
    }

    private var eyebrow: String {
        switch updater.phase {
        case .scanning, .connecting, .preparing, .pin: return "CONNECT YOUR PUFFIN"
        case .transferring: return "A FRESH START"
        case .complete: return "ONE LAST MOMENT"
        case .failed: return "LET’S TRY AGAIN"
        default: return "PUFFIN UPDATES"
        }
    }

    private var title: String {
        switch updater.phase {
        case .idle: return updater.release == nil ? "Your Puffin.\nEven better." : "Good things\nare on the way."
        case .importing: return "Getting things\nready."
        case .scanning, .connecting, .preparing: return "Hello, Puffin."
        case .pin: return "A quick hello."
        case .transferring: return finalizing ? "Finishing up." : "A little more\nPuffin."
        case .complete: return "Over to Puffin."
        case .failed: return updater.failureStage == .service ? "Updates are\nunavailable." : "We couldn’t\nfinish that."
        }
    }

    private var subtitle: String {
        switch updater.phase {
        case .idle: return updater.image != nil ? "Your local update is ready. Let’s connect your Puffin." : "The latest improvements, delivered wirelessly. No cables. No fuss."
        case .importing: return "We’ll take care of the details."
        case .scanning: return "Hold Fn + Up as you restart Puffin. Then choose it below."
        case .connecting, .preparing: return "Making a connection. Keep Puffin nearby."
        case .pin: return "Enter the six-digit code on Puffin’s screen to begin the update."
        case .transferring: return "Keep Puffin powered, nearby, and this app open. We’ll handle the rest."
        case .complete: return "Your update has been sent and checked. Puffin is restarting—check its screen when it’s back."
        case .failed:
            return updater.failureStage == .service
                ? "We couldn’t prepare your update. Check your internet connection and try again in a moment."
                : "Keep Puffin nearby and restart it in update mode. If it’s already restarting, check its screen before trying again."
        }
    }

    private var buttonTitle: String {
        switch updater.phase {
        case .pin: return "Update Puffin"
        case .complete: return "Done"
        case .failed: return "Try again"
        default: return updater.image != nil ? "Connect Puffin" : updater.release == nil ? "Check for updates" : "Get update"
        }
    }

    private func primaryAction() {
        pinFocused = false
        if updater.preview { updater.advancePreview(); return }
        switch updater.phase {
        case .pin: updater.authenticate(pin: pin); pin = ""
        case .complete: updater.cancel()
        default:
            if updater.image != nil { updater.scan() }
            else if updater.release != nil { updater.downloadUpdate() }
            else { updater.checkForUpdates() }
        }
    }
}
