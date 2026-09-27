import Foundation
import Testing

@testable import NotProtonApp

@Suite("Prefix environment settings")
struct PrefixEnvironmentStoreTests {

    private func samplePrefix(in dir: URL) -> WinePrefix {
        WinePrefix(
            appID: "1574480",
            name: "Agent 64: Spies Never Die",
            library: SteamLibrary(root: dir),
            lastUsed: nil
        )
    }

    @Test("Parses multiline assignments and comments")
    func parsesAssignments() throws {
        let values = try PrefixEnvironmentStore.parse(
            """
            # graphics
            DXVK_HUD=full
            WINEDEBUG=-all
            MANGOHUD=1
            """
        )

        #expect(values == [
            "DXVK_HUD": "full",
            "WINEDEBUG": "-all",
            "MANGOHUD": "1",
        ])
    }

    @Test("The value may contain equals signs and spaces")
    func keepsValueVerbatim() throws {
        let values = try PrefixEnvironmentStore.parse(
            "NOTPROTON_EXAMPLE=hello world=again"
        )
        #expect(values["NOTPROTON_EXAMPLE"] == "hello world=again")
    }

    @Test("Names with spaces and non-ASCII characters are rejected")
    func rejectsNonShellNames() throws {
        #expect(throws: PrefixEnvironmentStore.ValidationError.invalidLine(
            number: 1,
            text: "DXVK_HUD =1"
        )) {
            try PrefixEnvironmentStore.parse("DXVK_HUD =1")
        }

        #expect(throws: PrefixEnvironmentStore.ValidationError.invalidLine(
            number: 1,
            text: "ÉNV=1"
        )) {
            try PrefixEnvironmentStore.parse("ÉNV=1")
        }
    }

    @Test("Invalid assignments are rejected with their line number")
    func rejectsInvalidAssignments() throws {
        #expect(throws: PrefixEnvironmentStore.ValidationError.invalidLine(
            number: 2,
            text: "not-an-environment"
        )) {
            try PrefixEnvironmentStore.parse(
                """
                DXVK_HUD=1
                not-an-environment
                """
            )
        }
    }

    @Test("Critical runner variables cannot be overridden")
    func rejectsManagedVariables() throws {
        #expect(throws: PrefixEnvironmentStore.ValidationError.protectedName(
            number: 1,
            name: "WINEPREFIX"
        )) {
            try PrefixEnvironmentStore.parse("WINEPREFIX=/tmp/no")
        }
    }

    @Test("Saving and clearing is scoped to one prefix")
    func savesAndClears() throws {
        let root = FileManager.default.temporaryDirectory
            .appending(path: "np-env-\(UUID().uuidString)")
        let prefix = samplePrefix(in: root)
        try FileManager.default.createDirectory(at: prefix.root, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: root) }

        try PrefixEnvironmentStore.save(
            """
            DXVK_HUD=1
            WINEDEBUG=-all
            """,
            for: prefix
        )

        #expect(
            try PrefixEnvironmentStore.read(for: prefix)
                == "DXVK_HUD=1\nWINEDEBUG=-all\n"
        )
        #expect(PrefixEnvironmentStore.values(for: prefix) == [
            "DXVK_HUD": "1",
            "WINEDEBUG": "-all",
        ])

        try PrefixEnvironmentStore.save("", for: prefix)
        #expect(try PrefixEnvironmentStore.read(for: prefix) == "")
        #expect(
            FileManager.default.fileExists(
                atPath: PrefixEnvironmentStore.fileURL(for: prefix).path(percentEncoded: false)
            ) == false
        )
    }

    @Test("A missing settings file means no extra environment")
    func missingFileIsEmpty() throws {
        let root = FileManager.default.temporaryDirectory
            .appending(path: "np-env-missing-\(UUID().uuidString)")
        let prefix = samplePrefix(in: root)
        defer { try? FileManager.default.removeItem(at: root) }

        #expect(try PrefixEnvironmentStore.read(for: prefix) == "")
        #expect(PrefixEnvironmentStore.values(for: prefix).isEmpty)
    }
}
