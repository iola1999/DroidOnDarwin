// SPDX-License-Identifier: GPL-2.0-or-later
// Adapted from Leviidev/Husk, with contributions by Dan Perks. See ACKNOWLEDGEMENTS.md.
import Foundation

/// Codable messages shared by DroidOnDarwin and its JIT helper extension. Compiled into
/// both targets. Each travels as JSON in an extension request's item
/// (userInfo[itemKey]), to the helper and back.
struct JITRequest: Codable, Sendable {
    static let itemKey = "dod-jit-request"

    let targetPID: Int32
    let pairingData: Data
    let scriptBase64: String

    struct Response: Codable, Sendable {
        static let itemKey = "dod-jit-response"
        let success: Bool
        let message: String
    }
}
