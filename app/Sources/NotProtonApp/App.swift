// Self-explanatory

import AppKit
import SwiftUI

@main
struct NotProtonApp: App {

    @State private var status = SystemStatus()
    @State private var prefixes = PrefixesModel()
    @State private var pane: Pane = .status
    private let updater = AppUpdater()

    init() {
        AppLog.start()
    }

    var body: some Scene {
        Window("NotProton", id: "main") {
            RootView(pane: $pane)
                .environment(status)
                .environment(prefixes)
                .frame(minWidth: 720, minHeight: 460)
                .onChange(of: pane) { prefixes.forgetOutcome() }
                .onOpenURL { handleExternalURL($0) }
        }
        .defaultSize(width: 900, height: 760)
        .commands { menus }
    }

    @MainActor
    private func handleExternalURL(_ url: URL) {
        guard url.scheme == "notproton", url.host == "rebuild-prefix",
              let components = URLComponents(url: url, resolvingAgainstBaseURL: false)
        else { return }

        let values = Dictionary(
            uniqueKeysWithValues: (components.queryItems ?? []).compactMap { item in
                item.value.map { (item.name, $0) }
            }
        )

        guard let runnerName = values["runner"],
              let encodedPath = values["path64"],
              let data = Data(base64Encoded: encodedPath
                .replacingOccurrences(of: "-", with: "+")
                .replacingOccurrences(of: "_", with: "/")
                    + String(repeating: "=", count: (4 - encodedPath.count % 4) % 4)),
              let dataPath = String(data: data, encoding: .utf8)
        else {
            AppLog.note("external rebuild request was malformed")
            return
        }

        let build = SupportedRunners.all.first {
            runnerName == ($0.flavor ?? "rosetta")
        }
        guard let build else {
            AppLog.note("external rebuild request named unknown runner " + runnerName)
            return
        }

        let dataURL = URL(filePath: dataPath)
        let appID = dataURL.lastPathComponent
        let libraryRoot = dataURL
            .deletingLastPathComponent()
            .deletingLastPathComponent()
            .deletingLastPathComponent()
        let library = SteamLibrary(root: libraryRoot)
        let prefix = WinePrefix(
            appID: appID,
            name: PrefixStore.appName(appID: appID, in: library),
            library: library,
            lastUsed: nil
        )

        guard FileManager.default.fileExists(
            atPath: prefix.pfx.path(percentEncoded: false)
        ) else {
            AppLog.note("external rebuild request found no prefix at " + prefix.pfx.path(percentEncoded: false))
            return
        }

        let runner = SupportPaths.clonedRoot(forBuild: build.id)
        guard FileManager.default.fileExists(
            atPath: runner.appending(path: "lib/wine").path(percentEncoded: false)
        ) else {
            AppLog.note("external rebuild request has no installed runner " + build.id)
            return
        }

        Task {
            NSApp.activate(ignoringOtherApps: true)
            do {
                _ = try await Task.detached(priority: .userInitiated) {
                    try PrefixTools.recreate(prefix, runner: runner, keepBackup: true)
                }.value
                AppLog.note(
                    "external rebuild succeeded: app " + appID + ", runner " + build.displayVersion
                )
            } catch {
                AppLog.note(
                    "external rebuild failed: app " + appID + ", runner " + build.displayVersion + ": "
                        + error.localizedDescription
                )
                let alert = NSAlert()
                alert.messageText = "Prefix rebuild failed"
                alert.informativeText = error.localizedDescription
                alert.alertStyle = .critical
                alert.addButton(withTitle: "OK")
                alert.runModal()
            }
        }
    }

    @CommandsBuilder
    private var menus: some Commands {
        CommandGroup(replacing: .newItem) {}

        CommandGroup(after: .appInfo) {
            Button("Check for Updates…") { updater.check() }
        }

        CommandGroup(after: .sidebar) {
            Button("Status") { pane = .status }
                .keyboardShortcut("1", modifiers: .command)
            Button("Prefixes") { pane = .prefixes }
                .keyboardShortcut("2", modifiers: .command)
            Button("Prefix Backups") { pane = .backups }
                .keyboardShortcut("3", modifiers: .command)

            Divider()

            Button("Refresh") {
                Task {
                    switch pane {
                    case .status: await status.refresh()
                    case .prefixes, .backups: await prefixes.load()
                    }
                }
            }
            .keyboardShortcut("r", modifiers: .command)
            .disabled(pane == .status ? !status.isIdle : prefixes.isLoading)

            Divider()
        }

        CommandMenu("Setup") {
            Button("Install") { Task { await status.requestInstall() } }
                .keyboardShortcut("i", modifiers: .command)
                .disabled(!status.isIdle)

            Button("Set Up Compatibility Tool") { Task { await status.requestCompatibilityTool() } }
                .disabled(!status.isIdle || status.setupSource == nil)

            Button("Fetch Valve Binaries") { Task { await status.fetchValveBinaries() } }
                .disabled(!status.isIdle)

            Divider()

            updateBlockCommand

            Button("Repair Steam") { status.pendingConfirmation = .replaceSteam }
                .disabled(!status.isIdle)

            Divider()

            Button("Reveal Log in Finder") {
                NSWorkspace.shared.activateFileViewerSelecting([AppLog.file])
            }
        }

        prefixMenu
    }

    @CommandsBuilder
    private var prefixMenu: some Commands {
        CommandMenu("Prefix") {
            Button("Run Program…") {
                if let prefix = prefixTarget { prefixes.chooseExecutable(for: prefix) }
            }
            .disabled(prefixTarget == nil)

            Divider()

            ForEach(WineTool.allCases, id: \.self) { tool in
                Button(tool.label) {
                    if let prefix = prefixes.selectedPrefix { prefixes.open(tool, for: prefix) }
                }
                .disabled(prefixTarget == nil)
            }

            Divider()

            Button("Reveal in Finder") {
                if let prefix = prefixes.selectedPrefix { prefixes.reveal(prefix) }
            }
            .keyboardShortcut("r", modifiers: [.command, .shift])
            .disabled(prefixes.selectedPrefix == nil)

            Divider()

            Button(PrefixPrompt.backUpButton(selectionTargets)) {
                if !selectionTargets.isEmpty {
                    prefixes.pendingConfirmation = .backUp(selectionTargets)
                }
            }
            .disabled(selectionTargets.isEmpty)

            Button(PrefixPrompt.rebuildButton(selectionTargets)) {
                if !selectionTargets.isEmpty {
                    prefixes.pendingConfirmation = .rebuild(selectionTargets)
                }
            }
            .disabled(selectionTargets.isEmpty)

            Button(PrefixPrompt.deleteButton(selectionTargets)) {
                if !selectionTargets.isEmpty {
                    prefixes.pendingConfirmation = .delete(selectionTargets)
                }
            }
            .disabled(selectionTargets.isEmpty)
        }
    }

    private var prefixTarget: WinePrefix? {
        prefixes.isBusy ? nil : prefixes.selectedPrefix
    }

    private var selectionTargets: [WinePrefix] {
        prefixes.isBusy ? [] : prefixes.selectedPrefixes
    }

    @ViewBuilder
    private var updateBlockCommand: some View {
        if status.snapshot?.updateBlocked == true {
            Button("Allow Client Updates") { Task { await status.setUpdateBlock(false) } }
                .disabled(!status.isIdle)
        } else {
            Button("Block Client Updates") { status.pendingConfirmation = .blockUpdates }
                .disabled(!status.isIdle || status.snapshot == nil)
        }
    }
}
