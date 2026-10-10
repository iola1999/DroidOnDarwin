// SPDX-License-Identifier: GPL-2.0-or-later
import Foundation

/// The RPPairing file Built-in StikJIT authenticates with. Lives in Documents,
/// so it is visible through Finder file sharing -- it is device-sensitive and
/// is only ever sent to DroidOnDarwin's own helper process.
enum JITPairingFileStore {
    static var directory: URL {
        FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("StikJIT", isDirectory: true)
    }
    static var url: URL {
        directory.appendingPathComponent("pairingFile.plist")
    }
    static var exists: Bool {
        FileManager.default.fileExists(atPath: url.path)
    }
    static func data() throws -> Data {
        try Data(contentsOf: url)
    }

    static func importFile(from source: URL) throws {
        let scoped = source.startAccessingSecurityScopedResource()
        defer {
            if scoped {
                source.stopAccessingSecurityScopedResource()
            }
        }
        try store(try Data(contentsOf: source))
    }

    /// Validates an RPPairing plist and makes it the pairing file.
    private static func store(_ data: Data) throws {
        guard !data.isEmpty,
              let plist = try? PropertyListSerialization.propertyList(from: data, options: [], format: nil),
              let dictionary = plist as? [String: Any],
              let publicKey = dictionary["public_key"] as? Data, publicKey.count == 32,
              let privateKey = dictionary["private_key"] as? Data, privateKey.count == 32,
              let identifier = dictionary["identifier"] as? String, !identifier.isEmpty else {
            throw NSError(domain: "DroidOnDarwinJIT", code: 10,
                          userInfo: [NSLocalizedDescriptionKey:
                              "That is not a remote pairing file. Make one with the StikDebug pairing-file guide and try again."])
        }
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        try data.write(to: url, options: .atomic)
    }
}
