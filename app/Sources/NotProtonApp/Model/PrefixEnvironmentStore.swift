import Foundation

enum PrefixEnvironmentStore {

    static let fileName = "notproton-environment"

    private static let protectedNames: Set<String> = [
        "CX_ROOT",
        "CX_HOME",
        "PATH",
        "WINELOADER",
        "WINESERVER",
        "WINEDLLPATH",
        "WINEPREFIX",
        "STEAM_COMPAT_DATA_PATH",
        "STEAM_COMPAT_INSTALL_PATH",
        "STEAM_COMPAT_APP_ID",
        "STEAM_COMPAT_CLIENT_INSTALL_PATH",
        "SteamAppId",
        "SteamGameId",
    ]

    enum ValidationError: LocalizedError, Equatable {
        case invalidLine(number: Int, text: String)
        case protectedName(number: Int, name: String)

        var errorDescription: String? {
            switch self {
            case let .invalidLine(number, text):
                "Line \(number) is not a valid environment assignment: \(text)"
            case let .protectedName(number, name):
                "Line \(number) sets \(name), which NotProton manages automatically."
            }
        }
    }

    static func fileURL(for prefix: WinePrefix) -> URL {
        prefix.root.appending(path: fileName)
    }

    static func read(for prefix: WinePrefix) throws -> String {
        let url = fileURL(for: prefix)
        guard FileManager.default.fileExists(atPath: url.path(percentEncoded: false)) else {
            return ""
        }
        return try String(contentsOf: url, encoding: .utf8)
    }

    static func values(for prefix: WinePrefix) -> [String: String] {
        guard let text = try? read(for: prefix), let parsed = try? parse(text) else {
            return [:]
        }
        return parsed
    }

    static func parse(_ text: String) throws -> [String: String] {
        let normalized = text
            .replacingOccurrences(of: "\r\n", with: "\n")
            .replacingOccurrences(of: "\r", with: "\n")

        var values: [String: String] = [:]
        for (offset, rawLine) in normalized.split(separator: "\n", omittingEmptySubsequences: false).enumerated() {
            let number = offset + 1
            let line = String(rawLine)

            guard !line.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty,
                  !line.hasPrefix("#")
            else {
                continue
            }

            guard let equal = line.firstIndex(of: "=") else {
                throw ValidationError.invalidLine(number: number, text: line)
            }

            let rawName = String(line[..<equal]).trimmingCharacters(in: .whitespaces)
            let valueStart = line.index(after: equal)
            let value = String(line[valueStart...])

            guard isEnvironmentName(rawName) else {
                throw ValidationError.invalidLine(number: number, text: line)
            }

            if protectedNames.contains(rawName) {
                throw ValidationError.protectedName(number: number, name: rawName)
            }

            values[rawName] = value
        }

        return values
    }

    static func save(_ text: String, for prefix: WinePrefix) throws {
        _ = try parse(text)

        let url = fileURL(for: prefix)
        let normalized = text
            .replacingOccurrences(of: "\r\n", with: "\n")
            .replacingOccurrences(of: "\r", with: "\n")

        if normalized.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty {
            if FileManager.default.fileExists(atPath: url.path(percentEncoded: false)) {
                try FileManager.default.removeItem(at: url)
            }
            return
        }

        try normalized.write(to: url, atomically: true, encoding: .utf8)
    }

    private static func isEnvironmentName(_ name: String) -> Bool {
        guard let first = name.first,
              first == "_" || first.isLetter
        else {
            return false
        }

        return name.dropFirst().allSatisfy { character in
            character == "_" || character.isLetter || character.isNumber
        }
    }
}
