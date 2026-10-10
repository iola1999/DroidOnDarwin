// SPDX-License-Identifier: GPL-2.0-or-later
import Foundation

struct RunConfiguration: Codable, Equatable, Sendable {
    var nterp = false
    var expandedJIT = false
    var fastBaseline = false
    var aot = true
    var bootImage = false
    var directFault = true
    var faultPrefetch = false
    var sparseScan = true
    var performanceLogging = false
    var diagnostics = false

    static let keys: Set<String> = ["nterp", "expandedJIT", "fastBaseline", "aot", "bootImage", "directFault", "faultPrefetch", "sparseScan", "performanceLogging", "diagnostics"]
    static let environmentKeys = ["NCE_ART_NTERP", "NCE_ART_EXPANDED_JIT", "NCE_ART_FAST_BASELINE", "NCE_ART_AOT", "NCE_ART_BOOT_IMAGE", "NCE_DIRECT_FAULT", "NCE_FAULT_PREFETCH", "NCE_SPARSE_SCAN", "NCE_MAP_PROFILE", "NCE_DIAGNOSTICS"]

    static func saved(_ defaults: UserDefaults = .standard, diagnostics: Bool = false) -> Self {
        Self(nterp: defaults.bool(forKey: "artNterp"),
             expandedJIT: defaults.bool(forKey: "artExpandedJIT"),
             fastBaseline: defaults.bool(forKey: "artFastBaseline"),
             aot: defaults.object(forKey: "artAOT") as? Bool ?? true,
             bootImage: defaults.bool(forKey: "artBootImage"),
             directFault: defaults.object(forKey: "directFault") as? Bool ?? true,
             faultPrefetch: defaults.bool(forKey: "faultPrefetch"),
             sparseScan: defaults.object(forKey: "sparseScan") as? Bool ?? true,
             performanceLogging: defaults.bool(forKey: "performanceLogging"), diagnostics: diagnostics)
    }

    var environment: [String: String] {
        var values = ["NCE_ART_NTERP": nterp ? "1" : "0", "NCE_DIRECT_FAULT": directFault ? "1" : "0",
                      "NCE_ART_EXPANDED_JIT": expandedJIT ? "1" : "0",
                      "NCE_ART_FAST_BASELINE": fastBaseline ? "1" : "0",
                      "NCE_ART_AOT": aot ? "1" : "0",
                      "NCE_ART_BOOT_IMAGE": bootImage ? "1" : "0",
                      "NCE_FAULT_PREFETCH": faultPrefetch ? "1" : "0",
                      "NCE_SPARSE_SCAN": sparseScan ? "1" : "0", "NCE_MAP_PROFILE": performanceLogging ? "1" : "0"]
        if diagnostics {
            values["NCE_DIAGNOSTICS"] = "1"
        }
        return values
    }

    func validateResources(_ resources: URL) throws {
        guard bootImage else {
            return
        }
        let url = resources.appendingPathComponent("Runtime.json")
        guard let data = try? Data(contentsOf: url),
              let metadata = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
              let build = metadata["build"] as? [String: Any],
              let image = build["art_boot"] as? [String: Any],
              let files = image["files"] as? [String: String],
              Set(files.keys) == ["boot.art", "boot.oat", "boot.vdex"] else {
            throw NSError(domain: "DroidOnDarwin.Configuration", code: 1,
                          userInfo: [NSLocalizedDescriptionKey: "当前构建缺少公共启动镜像，请关闭该选项或安装包含镜像的版本"])
        }
    }

    static func fromEnvironment(_ values: [String: String]) -> Self {
        Self(nterp: values["NCE_ART_NTERP"] == "1", expandedJIT: values["NCE_ART_EXPANDED_JIT"] == "1", fastBaseline: values["NCE_ART_FAST_BASELINE"] == "1", aot: values["NCE_ART_AOT"] != "0", bootImage: values["NCE_ART_BOOT_IMAGE"] == "1", directFault: values["NCE_DIRECT_FAULT"] != "0",
             faultPrefetch: values["NCE_FAULT_PREFETCH"] == "1", sparseScan: values["NCE_SPARSE_SCAN"] != "0", performanceLogging: values["NCE_MAP_PROFILE"] == "1",
             diagnostics: values["NCE_DIAGNOSTICS"] == "1")
    }
}
