import Foundation

// 测试进程内替换文档目录，磁盘处理仍调用 GuestImage 的实际实现。
final class FileManager: Foundation.FileManager, @unchecked Sendable {
    static let fixture = Foundation.FileManager.default.temporaryDirectory
        .appendingPathComponent("husk-guest-tests-\(UUID().uuidString)")
    private static let instance = FileManager()
    override class var `default`: Foundation.FileManager { instance }
    override func urls(for directory: SearchPathDirectory, in domainMask: SearchPathDomainMask) -> [URL] {
        if directory == .documentDirectory { return [Self.fixture] }
        return super.urls(for: directory, in: domainMask)
    }
}

enum HuskLog {
    static func log(_ category: String, _ message: String) {}
}

@main
struct GuestImageTests {
    @MainActor static func main() async throws {
        let root = FileManager.fixture
        let fm = Foundation.FileManager.default
        try fm.createDirectory(at: root, withIntermediateDirectories: true)
        defer { try? fm.removeItem(at: root) }
        func write(_ name: String, _ text: String) throws {
            try Data(text.utf8).write(to: root.appendingPathComponent(name))
        }
        func require(_ condition: @autoclosure () -> Bool, _ message: String) {
            if !condition() { fatalError(message) }
        }
        let guest = GuestImage()
        let disk = root.appendingPathComponent("lineage-vda.qcow2")
        fm.createFile(atPath: disk.path, contents: Data([0x51, 0x46, 0x49, 0xfb]))
        let file = try FileHandle(forWritingTo: disk)
        try file.truncate(atOffset: 64 * 1024 * 1024)
        try file.close()
        try write("husk-custom-guest", "hyperos-3\n")
        try write("lineage-guest.version", "older-version")
        try write("lineage-vdb.qcow2", "existing user data")
        try write("lineage-vdb.qcow2.seed", "older-seed")
        try write("edk2-aarch64-code.fd", "test firmware")
        try write("lineage-efi-vars.fd", "test variables")
        try write("lineage-efi-vars.fd.layout", GuestImage.deviceLayoutSignature)

        guest.refresh()
        require(guest.state == .ready, "imported system survives a version mismatch")
        require(!GuestImage.wantsSnapshot, "imported system does not request a release snapshot")
        try guest.prepareFirmware()
        let userdata = try String(contentsOfFile: guest.userdataPath, encoding: .utf8)
        require(userdata == "existing user data",
                "an old seed stamp must preserve imported userdata")
        await guest.checkForUpdates()
        require(guest.update == .none, "imported system has no release update")
        guest.downloadSnapshotNow()
        require(guest.state == .ready && !guest.isFetchingSnapshot, "snapshot action preserves the guest")
        guest.download()
        require(fm.fileExists(atPath: guest.diskPath), "download action preserves the imported disk")

        try fm.removeItem(atPath: guest.userdataPath)
        do {
            try guest.prepareFirmware()
            fatalError("missing imported userdata must report an error")
        } catch {}
        require(!fm.fileExists(atPath: guest.userdataPath), "missing userdata must stay missing")
        try write("lineage-vda.qcow2", "invalid disk kept for recovery")
        guest.refresh()
        require(fm.fileExists(atPath: guest.diskPath), "invalid imported image remains available for recovery")
        try fm.removeItem(at: disk)
        guest.refresh()
        if case .failed = guest.state {} else { fatalError("missing custom image must report an error") }

        // 同一套代码继续保留官方镜像原有的版本检查行为。
        try fm.removeItem(at: root.appendingPathComponent("husk-custom-guest"))
        try write("lineage-vda.qcow2", "stale stock image")
        guest.refresh()
        require(guest.state == .missing && !fm.fileExists(atPath: guest.diskPath),
                "stock image version checks must remain active")
        print("Guest image preservation tests passed")
    }
}
