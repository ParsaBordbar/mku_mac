// The Mac launcher: choose your ISO, press Play.
//
// A thin window over ./mku-play, which does the real work: checking the ISO and
// translating the game for this computer. The app contains no game code. What
// it builds goes to ~/Library/Application Support/MK Unchained/, with the saves.
//
// Run with --selftest ISO to go through the same steps without a window.
import AppKit
import SwiftUI
import UniformTypeIdentifiers

enum Stage: Equatable {
    case chooseISO
    case needAppleTools
    case working(String)
    case ready(String)
    case playing
    case failed(String)
}

/// The steps themselves, shared by the window and --selftest.
struct Backend {
    let resources: URL
    let support: URL
    var game: URL { support.appendingPathComponent("game") }

    init() {
        resources = Bundle.main.resourceURL!.appendingPathComponent("game")
        let base = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
        support = base.appendingPathComponent("MK Unchained")
    }

    var appleToolsInstalled: Bool {
        run("/usr/bin/xcode-select", ["-p"], in: nil).status == 0
            && FileManager.default.isExecutableFile(atPath: "/usr/bin/python3")
            && run("/usr/bin/python3", ["-c", "pass"], in: nil).status == 0
    }

    func askForAppleTools() { _ = run("/usr/bin/xcode-select", ["--install"], in: nil) }

    /// Unpack the translator next to where it will build. Keeps build/, saves/ and mku.ini.
    func unpack() -> String? {
        try? FileManager.default.createDirectory(at: game, withIntermediateDirectories: true)
        let r = run("/usr/bin/ditto", ["--noqtn", resources.path, game.path], in: nil)
        return r.status == 0 ? nil : "Couldn't set up the game folder.\n\(r.output)"
    }

    func play(_ args: [String], extraEnv: [String: String] = [:], log: URL? = nil) -> (status: Int32, output: String) {
        run("/usr/bin/python3", ["-I", "-u", "mku-play"] + args, in: game, extraEnv: extraEnv, log: log)
    }

    func run(_ tool: String, _ args: [String], in dir: URL?, extraEnv: [String: String] = [:], log: URL? = nil) -> (status: Int32, output: String) {
        let p = Process()
        p.executableURL = URL(fileURLWithPath: tool)
        p.arguments = args
        if let dir { p.currentDirectoryURL = dir }
        var env = ProcessInfo.processInfo.environment
        env["PATH"] = "/usr/bin:/bin:/usr/sbin:/sbin"
        for (k, v) in extraEnv { env[k] = v }
        p.environment = env
        var captured = Data()
        let group = DispatchGroup()
        if let log {
            FileManager.default.createFile(atPath: log.path, contents: nil)
            let h = try? FileHandle(forWritingTo: log)
            p.standardOutput = h
            p.standardError = h
        } else {
            let pipe = Pipe()
            p.standardOutput = pipe
            p.standardError = pipe
            group.enter()
            DispatchQueue.global().async {
                captured = pipe.fileHandleForReading.readDataToEndOfFile()
                group.leave()
            }
        }
        do { try p.run() } catch { return (127, "\(error.localizedDescription)") }
        p.waitUntilExit()
        group.wait()
        return (p.terminationStatus, String(decoding: captured, as: UTF8.self))
    }

    /// The last paragraph mku-play printed, without the file's path on the line before.
    static func reason(_ output: String) -> String {
        var lines = output.split(separator: "\n", omittingEmptySubsequences: true).map(String.init)
        if lines.count > 1, lines[0].hasPrefix("/") { lines.removeFirst() }
        return lines.joined(separator: "\n")
    }
}

@MainActor
final class Model: ObservableObject {
    @Published var stage: Stage = .chooseISO
    @Published var scale: Int = UserDefaults.standard.object(forKey: "scale") as? Int ?? 2
    @Published var textures: Int = UserDefaults.standard.object(forKey: "textures") as? Int ?? 1
    @Published var dropping = false
    private(set) var iso: String? = UserDefaults.standard.string(forKey: "iso")
    private let backend = Backend()
    private var waitingForTools = false

    var isoName: String { iso.map { ($0 as NSString).lastPathComponent } ?? "" }

    func start() {
        if let iso, FileManager.default.fileExists(atPath: iso) { prepare(iso) }
    }

    func chooseISO() {
        let panel = NSOpenPanel()
        panel.message = "Choose the ISO of your own copy of the game"
        panel.allowedContentTypes = [UTType(filenameExtension: "iso") ?? .data]
        if panel.runModal() == .OK, let url = panel.url { prepare(url.path) }
    }

    func prepare(_ path: String) {
        stage = .working("Checking your game…")
        iso = path
        let backend = self.backend
        Task.detached {
            if !backend.appleToolsInstalled {
                backend.askForAppleTools()
                await MainActor.run { self.stage = .needAppleTools; self.waitForTools(path) }
                return
            }
            if let problem = backend.unpack() {
                await MainActor.run { self.stage = .failed(problem) }
                return
            }
            let check = backend.play(["--check", path])
            guard check.status == 0 else {
                await MainActor.run {
                    self.iso = nil
                    UserDefaults.standard.removeObject(forKey: "iso")
                    self.stage = .failed(Backend.reason(check.output))
                }
                return
            }
            let title = check.output.components(separatedBy: "Found: ").last?
                .trimmingCharacters(in: .whitespacesAndNewlines) ?? "Your game"
            await MainActor.run {
                UserDefaults.standard.set(path, forKey: "iso")
                self.stage = .working("Getting the game ready for this Mac…\nThe first time takes a few minutes.")
            }
            let built = backend.play(["--build", path])
            await MainActor.run {
                self.stage = built.status == 0 ? .ready(title) : .failed("The game couldn't be built.\n\n" + String(built.output.suffix(800)))
            }
        }
    }

    private func waitForTools(_ path: String) {
        let backend = self.backend
        Task.detached {
            while !backend.appleToolsInstalled {
                try? await Task.sleep(nanoseconds: 3_000_000_000)
            }
            await MainActor.run { self.prepare(path) }
        }
    }

    func play() {
        guard let iso else { return }
        UserDefaults.standard.set(scale, forKey: "scale")
        UserDefaults.standard.set(textures, forKey: "textures")
        stage = .playing
        let backend = self.backend
        let (scale, textures) = (scale, textures)
        let log = backend.support.appendingPathComponent("last-run.log")
        Task.detached {
            let r = backend.play(["--scale", "\(scale)", "--textures", "\(textures)", iso], log: log)
            await MainActor.run {
                self.stage = r.status == 0 ? .ready(self.isoName)
                    : .failed("The game stopped unexpectedly.\nDetails are in:\n\(log.path)")
            }
        }
    }

    func reset() {
        stage = .chooseISO
    }
}

struct ContentView: View {
    @ObservedObject var model: Model
    var autoStart = true

    var body: some View {
        VStack(spacing: 16) {
            Text("Mortal Kombat: Unchained").font(.title2).bold()
            content.frame(maxWidth: .infinity, maxHeight: .infinity)
            Text("You need your own copy of the game. No game files come with this app.")
                .font(.footnote).foregroundStyle(.secondary).multilineTextAlignment(.center)
        }
        .padding(24)
        .frame(width: 440, height: 360)
        .onAppear { if autoStart { model.start() } }
    }

    @ViewBuilder private var content: some View {
        switch model.stage {
        case .chooseISO:
            VStack(spacing: 12) {
                Image(systemName: "opticaldisc").font(.system(size: 44)).foregroundStyle(.secondary)
                Text("Drop your game's ISO file here")
                Button("Choose File…") { model.chooseISO() }.keyboardShortcut(.defaultAction)
            }
            .frame(maxWidth: .infinity, maxHeight: .infinity)
            .background(RoundedRectangle(cornerRadius: 12)
                .strokeBorder(model.dropping ? Color.accentColor : Color.secondary.opacity(0.4),
                              style: StrokeStyle(lineWidth: 2, dash: [6])))
            .onDrop(of: [.fileURL], isTargeted: $model.dropping) { providers in
                guard let item = providers.first else { return false }
                _ = item.loadObject(ofClass: URL.self) { url, _ in
                    if let url { Task { @MainActor in model.prepare(url.path) } }
                }
                return true
            }
        case .needAppleTools:
            VStack(spacing: 12) {
                Text("One free install from Apple").font(.headline)
                Text("Click **Install** in the window that just opened. This app continues by itself when it's finished, which can take several minutes.")
                    .multilineTextAlignment(.center)
                ProgressView()
            }
        case .working(let message):
            VStack(spacing: 14) {
                ProgressView()
                Text(message).multilineTextAlignment(.center)
            }
        case .ready(let title):
            VStack(spacing: 14) {
                Text(title).foregroundStyle(.secondary).multilineTextAlignment(.center)
                Button { model.play() } label: {
                    Text("Play").font(.title3).frame(width: 160, height: 32)
                }
                .keyboardShortcut(.defaultAction)
                Form {
                    Picker("Resolution", selection: $model.scale) {
                        Text("Original (480×272)").tag(1)
                        Text("HD (960×544)").tag(2)
                        Text("Higher (1440×816)").tag(3)
                        Text("Highest (1920×1088)").tag(4)
                    }
                    Picker("Textures", selection: $model.textures) {
                        Text("Original").tag(1)
                        Text("Smooth").tag(2)
                        Text("Smoother").tag(4)
                    }
                }
                .frame(width: 320)
                Button("Use a different ISO…") { model.reset() }.buttonStyle(.link)
            }
        case .playing:
            VStack(spacing: 10) {
                Text("Playing").font(.headline)
                Text("Esc quits the game.").foregroundStyle(.secondary)
            }
        case .failed(let message):
            VStack(spacing: 14) {
                Image(systemName: "exclamationmark.triangle").font(.system(size: 32)).foregroundStyle(.orange)
                ScrollView { Text(message).multilineTextAlignment(.center).textSelection(.enabled) }
                Button("Try Another File…") { model.reset() }
            }
        }
    }
}

struct LauncherApp: App {
    @StateObject private var model = Model()

    var body: some Scene {
        WindowGroup { ContentView(model: model) }.windowResizability(.contentSize)
    }
}

/// Draw each screen to a PNG without showing anything, for tests: --snapshot DIR
@MainActor
func snapshots(into dir: String) {
    let screens: [(String, Stage)] = [
        ("choose", .chooseISO), ("tools", .needAppleTools),
        ("working", .working("Getting the game ready for this Mac…\nThe first time takes a few minutes.")),
        ("ready", .ready("Mortal Kombat: Unchained (USA, ULUS-10102, v1.03)")), ("playing", .playing),
        ("failed", .failed("unsupported game or version (disc id UCES00000, BOOT.BIN sha-256 0123456789abcdef...).\nSupported: Mortal Kombat: Unchained (USA, ULUS-10102, v1.03)")),
    ]
    _ = NSApplication.shared
    for (name, stage) in screens {
        let model = Model()
        model.stage = stage
        let view = NSHostingView(rootView: ContentView(model: model, autoStart: false))
        view.frame = NSRect(x: 0, y: 0, width: 440, height: 360)
        let window = NSWindow(contentRect: view.frame, styleMask: [.titled], backing: .buffered, defer: false)
        window.contentView = view
        window.appearance = NSAppearance(named: .aqua)   // readable on the PNG's transparent background
        view.layoutSubtreeIfNeeded()
        RunLoop.main.run(until: Date().addingTimeInterval(0.3))
        let rep = view.bitmapImageRepForCachingDisplay(in: view.bounds)!
        view.cacheDisplay(in: view.bounds, to: rep)
        try? rep.representation(using: .png, properties: [:])?.write(to: URL(fileURLWithPath: "\(dir)/\(name).png"))
    }
}

/// Every step but the window, for tests: --selftest ISO
func selftest(_ iso: String) -> Int32 {
    let b = Backend()
    print("apple tools:", b.appleToolsInstalled)
    guard b.appleToolsInstalled else { return 2 }
    if let e = b.unpack() { print(e); return 3 }
    let check = b.play(["--check", iso])
    print("check:", check.status, Backend.reason(check.output))
    guard check.status == 0 else { return 4 }
    let t0 = Date()
    let built = b.play(["--build", iso])
    print("build:", built.status, "in", Int(Date().timeIntervalSince(t0)), "s")
    guard built.status == 0 else { print(built.output); return 5 }
    let log = b.support.appendingPathComponent("last-run.log")
    let r = b.play(["--scale", "2", "--textures", "1", iso], log: log)
    print("play:", r.status, "log:", log.path)
    return r.status
}

@main
enum Main {
    static func main() {
        let args = CommandLine.arguments
        if let i = args.firstIndex(of: "--selftest"), i + 1 < args.count { exit(selftest(args[i + 1])) }
        if let i = args.firstIndex(of: "--snapshot"), i + 1 < args.count {
            MainActor.assumeIsolated { snapshots(into: args[i + 1]) }
            exit(0)
        }
        LauncherApp.main()
    }
}
