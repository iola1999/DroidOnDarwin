// SPDX-License-Identifier: GPL-2.0-or-later
import Foundation

struct RunConfiguration: Codable, Equatable, Sendable {
    var nterp = false
    var expandedJIT = false
    var directFault = true
    var sparseScan = true
    var performanceLogging = false
    var diagnostics = false

    static let keys: Set<String> = ["nterp", "expandedJIT", "directFault", "sparseScan", "performanceLogging", "diagnostics"]
    static let environmentKeys = ["NCE_ART_NTERP", "NCE_ART_EXPANDED_JIT", "NCE_DIRECT_FAULT", "NCE_SPARSE_SCAN", "NCE_MAP_PROFILE", "NCE_DIAGNOSTICS"]

    static func saved(_ defaults: UserDefaults = .standard, diagnostics: Bool = false) -> Self {
        Self(nterp: defaults.bool(forKey: "artNterp"),
             expandedJIT: defaults.bool(forKey: "artExpandedJIT"),
             directFault: defaults.object(forKey: "directFault") as? Bool ?? true,
             sparseScan: defaults.object(forKey: "sparseScan") as? Bool ?? true,
             performanceLogging: defaults.bool(forKey: "performanceLogging"), diagnostics: diagnostics)
    }

    var environment: [String: String] {
        var values = ["NCE_ART_NTERP": nterp ? "1" : "0", "NCE_DIRECT_FAULT": directFault ? "1" : "0",
                      "NCE_ART_EXPANDED_JIT": expandedJIT ? "1" : "0",
                      "NCE_SPARSE_SCAN": sparseScan ? "1" : "0", "NCE_MAP_PROFILE": performanceLogging ? "1" : "0"]
        if diagnostics {
            values["NCE_DIAGNOSTICS"] = "1"
        }
        return values
    }

    static func fromEnvironment(_ values: [String: String]) -> Self {
        Self(nterp: values["NCE_ART_NTERP"] == "1", expandedJIT: values["NCE_ART_EXPANDED_JIT"] == "1", directFault: values["NCE_DIRECT_FAULT"] != "0",
             sparseScan: values["NCE_SPARSE_SCAN"] != "0", performanceLogging: values["NCE_MAP_PROFILE"] == "1",
             diagnostics: values["NCE_DIAGNOSTICS"] == "1")
    }
}
