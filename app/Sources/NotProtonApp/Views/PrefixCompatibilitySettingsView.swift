import SwiftUI

struct PrefixCompatibilitySettingsView: View {

    let prefix: WinePrefix

    @Environment(\.dismiss) private var dismiss
    @State private var environmentText: String
    @State private var errorMessage: String?

    init(prefix: WinePrefix) {
        self.prefix = prefix
        _environmentText = State(initialValue: (try? PrefixEnvironmentStore.read(for: prefix)) ?? "")
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 16) {
            Text("Steam Compatibility Settings")
                .font(.title2)
                .fontWeight(.semibold)

            Text(prefix.title)
                .foregroundStyle(.secondary)

            VStack(alignment: .leading, spacing: 8) {
                Text("Environment Variables")
                    .font(.headline)

                Text("Set one NAME=VALUE pair per line. These values are stored with this prefix and loaded when Steam starts the game. Lines beginning with # are comments.")
                    .foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)

                ZStack(alignment: .topLeading) {
                    TextEditor(text: $environmentText)
                        .font(.system(.body, design: .monospaced))
                        .frame(minHeight: 190)
                        .padding(4)
                        .overlay {
                            RoundedRectangle(cornerRadius: 6)
                                .stroke(.quaternary, lineWidth: 1)
                        }

                    if environmentText.isEmpty {
                        Text("DXVK_HUD=1\nWINEDEBUG=-all\n# MANGOHUD=1")
                            .font(.system(.body, design: .monospaced))
                            .foregroundStyle(.tertiary)
                            .padding(.top, 12)
                            .padding(.leading, 10)
                            .allowsHitTesting(false)
                    }
                }

                if let errorMessage {
                    Label(errorMessage, systemImage: "exclamationmark.triangle")
                        .foregroundStyle(.red)
                        .fixedSize(horizontal: false, vertical: true)
                }

                Text("NotProton manages the Steam, Wine loader, runner path, and prefix variables automatically.")
                    .font(.caption)
                    .foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
            }

            Spacer(minLength: 0)

            HStack {
                Button("Cancel") {
                    dismiss()
                }
                .keyboardShortcut(.cancelAction)

                Spacer()

                Button("Save") {
                    save()
                }
                .keyboardShortcut(.defaultAction)
            }
        }
        .padding(24)
        .frame(minWidth: 560, minHeight: 420)
    }

    private func save() {
        do {
            try PrefixEnvironmentStore.save(environmentText, for: prefix)
            dismiss()
        } catch {
            errorMessage = (error as? LocalizedError)?.errorDescription ?? error.localizedDescription
        }
    }
}
