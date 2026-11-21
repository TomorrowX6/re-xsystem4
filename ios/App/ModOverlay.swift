import Foundation
import Darwin

enum ModError: LocalizedError {
    case message(String)
    var errorDescription: String? { if case .message(let text) = self { return text }; return nil }
}

enum ModFiles {
    static let files = FileManager.default
    static func children(_ url: URL) -> [URL] { (try? files.contentsOfDirectory(at: url, includingPropertiesForKeys: nil)) ?? [] }
    static func isFile(_ url: URL) -> Bool { (try? url.resourceValues(forKeys: [.isRegularFileKey]).isRegularFile) == true }
    static func isDirectory(_ url: URL) -> Bool { (try? url.resourceValues(forKeys: [.isDirectoryKey]).isDirectory) == true }
    static func exists(_ url: URL) -> Bool { files.fileExists(atPath: url.path) }
    static func fileSize(_ url: URL) -> Int64 { ((try? files.attributesOfItem(atPath: url.path)[.size]) as? NSNumber)?.int64Value ?? 0 }
    static func removeIfPresent(_ url: URL) throws { if exists(url) { try files.removeItem(at: url) } }
    static func walk(_ url: URL) throws -> [URL] {
        var result: [URL] = []
        for child in try files.contentsOfDirectory(at: url, includingPropertiesForKeys: [.isSymbolicLinkKey]) {
            if try child.resourceValues(forKeys: [.isSymbolicLinkKey]).isSymbolicLink == true { throw ModError.message("不支持符号链接：" + child.path) }
            result.append(child)
            if isDirectory(child) { result += try walk(child) }
        }
        return result
    }
    static func relative(_ url: URL, to root: URL) throws -> String {
        guard let relative = url.relativeFilePath(to: root) else { throw ModError.message("无效文件路径：" + url.path) }
        return relative
    }
    static func ini(_ directory: URL) -> [String: String] {
        let children = children(directory)
        let file = children.first { $0.lastPathComponent == "AliceStart.ini" } ?? children.first { $0.lastPathComponent == "System40.ini" } ?? children.first { ["alicestart.ini", "system40.ini"].contains($0.lastPathComponent.lowercased()) }
        return file.map(iniFile) ?? [:]
    }
    static func iniFile(_ file: URL) -> [String: String] {
        guard let data = try? Data(contentsOf: file) else { return [:] }
        let text = String(data: data, encoding: .utf8) ?? ModArchiveExtractor.decodeEntryPath(data)
        var result: [String: String] = [:]
        for line in text.components(separatedBy: .newlines) {
            let pieces = line.split(separator: "=", maxSplits: 1).map(String.init)
            if pieces.count == 2 { result[pieces[0].trimmingCharacters(in: .whitespaces).lowercased()] = pieces[1].trimmingCharacters(in: .whitespaces).trimmingCharacters(in: CharacterSet(charactersIn: "\"")) }
        }
        if result["gamename"] == nil { result["gamename"] = result["regist"] }
        return result
    }
    static func checked(_ root: URL, _ relative: String) throws -> URL {
        let parts = relative.replacingOccurrences(of: "\\", with: "/").split(separator: "/")
        guard !relative.hasPrefix("/"), !relative.hasPrefix("\\"), !parts.contains(".."), !relative.contains("\0"), parts.first?.contains(":") != true else { throw ModError.message("无效文件路径：" + relative) }
        var current = root
        for part in parts {
            current.appendPathComponent(String(part))
            if (try? files.destinationOfSymbolicLink(atPath: current.path)) != nil { throw ModError.message("不支持符号链接：" + relative) }
        }
        guard current.relativeFilePath(to: root) != nil else { throw ModError.message("无效文件路径：" + relative) }
        return current
    }
    static func replace(_ source: URL, _ destination: URL) throws {
        if rename(source.path, destination.path) == 0 { return }
        try removeIfPresent(destination)
        try files.copyItem(at: source, to: destination)
        try files.removeItem(at: source)
    }
    static func journal(_ lines: [String], to file: URL) throws {
        let temporary = file.appendingPathExtension("tmp")
        let data = Data(lines.joined(separator: "\n").utf8)
        if !exists(temporary) { guard files.createFile(atPath: temporary.path, contents: nil) else { throw ModError.message("无法写入 MOD 文件记录") } }
        let handle = try FileHandle(forWritingTo: temporary)
        defer { try? handle.close() }
        try handle.truncate(atOffset: 0)
        try handle.write(contentsOf: data)
        try handle.synchronize()
        guard rename(temporary.path, file.path) == 0 else { throw NSError(domain: NSPOSIXErrorDomain, code: Int(errno)) }
    }
}

enum ModOverlay {
    static let journalName = ".mod_installed_files.txt"
    static func detectInstalledMod(_ directory: URL) -> Bool {
        if ModFiles.isFile(directory.appendingPathComponent(InstalledModInfo.metaFileName)) || ModFiles.isFile(directory.appendingPathComponent(journalName)) { return true }
        for file in ModFiles.children(directory).filter(ModFiles.isFile) {
            let name = file.lastPathComponent.lowercased()
            if name.hasSuffix(".vanilla.bak") || (name.hasPrefix("rance10modcg") && name.hasSuffix(".afa")) || (name.hasPrefix("rance10modflat") && name.hasSuffix(".afa")) || ["asratest.ini", "作弊码.txt"].contains(name) { return true }
            if name == "rance10.ain", let size = try? file.resourceValues(forKeys: [.fileSizeKey]).fileSize, size > 8_000_000 { return true }
        }
        let ini = ModFiles.ini(directory), name = ini["gamename"] ?? ""
        return ini["savefolder"]?.lowercased() == "modsavedata" || name.contains("收费必是倒卖") || name.contains("日不落") || name.contains("[MOD")
    }
    static func canRestoreVanilla(_ directory: URL) -> Bool {
        ModFiles.isFile(directory.appendingPathComponent(journalName)) || ModFiles.children(directory).contains { $0.lastPathComponent.hasSuffix(".vanilla.bak") }
    }
    static func displayName(_ directory: URL, baseName: String? = nil) -> String {
        let info = InstalledModInfo.readOrInfer(directory)
        let raw = (baseName ?? ModFiles.ini(directory)["gamename"] ?? directory.lastPathComponent).replacingOccurrences(of: "兰斯１０", with: "兰斯10")
        guard info != nil || detectInstalledMod(directory) else { return raw }
        let backup = ["AliceStart.ini.vanilla.bak", "System40.ini.vanilla.bak"].map { directory.appendingPathComponent($0) }.first(where: ModFiles.exists)
        var base = (backup.flatMap { ModFiles.iniFile($0)["gamename"] } ?? ((raw.contains("收费必是倒卖") || raw.contains("日不落")) ? "兰斯10 决战" : raw)).replacingOccurrences(of: "兰斯１０", with: "兰斯10")
        if base.hasSuffix("[MOD]") { base = String(base.dropLast(5)) }
        let tag = info?.kind == "cn_patch" ? "汉化" : "MOD"
        return base.trimmingCharacters(in: .whitespacesAndNewlines) + " [" + tag + (info.map { " · " + $0.version } ?? "") + "]"
    }
    private static func isAsset(_ file: URL) -> Bool { ModFiles.isFile(file) && ["ain", "afa", "ex", "ald", "bgi", "wai", "fnl"].contains(file.pathExtension.lowercased()) }
    static func isStandaloneFullModDir(_ root: URL) -> Bool {
        let names = ModFiles.children(root).filter(ModFiles.isFile).map { $0.lastPathComponent.lowercased() }
        return names.contains(where: { $0.hasSuffix(".ain") }) && names.contains("rance10modcg.afa") && names.contains("rance10modflat.afa")
    }
    static func findModRootDir(_ staging: URL, isCnPatch: Bool = false) throws -> URL {
        var current = staging
        while true {
            let children = ModFiles.children(current)
            if children.contains(where: isAsset) { return current }
            let directories = children.filter { ModFiles.isDirectory($0) && !$0.lastPathComponent.hasPrefix(".") }
            if directories.count == 1 { current = directories[0] }
            else if let found = try directories.first(where: { try ModFiles.walk($0).contains(where: isAsset) }) { current = found }
            else { throw ModError.message(isCnPatch ? "未找到有效的汉化补丁文件" : "未找到有效的 MOD 文件") }
        }
    }
    static func isEvenicle2CnPatchJunkRelPath(_ relative: String) -> Bool {
        let name = relative.replacingOccurrences(of: "\\", with: "/").components(separatedBy: "/").last!.lowercased()
        return [".exe", ".dll", ".torrent"].contains(where: name.hasSuffix) || name == "staff.png" || (name.hasPrefix("请读我") && name.hasSuffix(".png"))
    }
    static func normalizeEvenicle2CnPatchStaging(_ staging: URL) throws {
        for mai in try ModFiles.walk(staging).filter({ ModFiles.isDirectory($0) && $0.lastPathComponent.lowercased() == "mai" }) {
            let children = ModFiles.children(mai)
            guard let sc = children.first(where: { ModFiles.isDirectory($0) && $0.lastPathComponent.lowercased() == "sc" }) else { continue }
            let scripts = ModFiles.children(sc).filter(ModFiles.isFile)
            guard scripts.contains(where: { ["ain", "ex"].contains($0.pathExtension.lowercased()) }) else { continue }
            for file in scripts { try ModFiles.replace(file, mai.deletingLastPathComponent().appendingPathComponent(file.lastPathComponent)) }
            for child in children where ModFiles.isDirectory(child) && ["sc", "enc", "org", "pic"].contains(child.lastPathComponent.lowercased()) && ModFiles.children(child).isEmpty { try ModFiles.files.removeItem(at: child) }
        }
    }
    static func resolveCaseInsensitiveDest(_ root: URL, _ relative: String) throws -> URL {
        let parts = relative.replacingOccurrences(of: "\\", with: "/").split(separator: "/")
        var current = root
        for (index, part) in parts.enumerated() {
            let name = String(part)
            let matched = ModFiles.children(current).first { child in
                child.lastPathComponent.lowercased() == name.lowercased() && (index == parts.count - 1 ? (ModFiles.isFile(child) && !child.lastPathComponent.hasSuffix(".vanilla.bak") && !child.lastPathComponent.hasSuffix(".vanilla.bak.tmp")) : ModFiles.isDirectory(child))
            }
            current = matched ?? current.appendingPathComponent(name)
            _ = try ModFiles.checked(root, ModFiles.relative(current, to: root))
        }
        return current
    }
    private static func pruneEmptyParentDirs(_ start: URL, stop: URL) throws {
        var current = start.canonicalFileURL
        let stop = stop.canonicalFileURL
        while current.path != stop.path && current.path.hasPrefix(stop.path + "/") && ModFiles.children(current).isEmpty {
            if !ModFiles.exists(current) { current.deleteLastPathComponent(); continue }
            try ModFiles.files.removeItem(at: current); current.deleteLastPathComponent()
        }
    }
    static func restoreVanilla(_ target: URL) throws {
        try InstalledModInfo.delete(target)
        for file in try ModFiles.walk(target).reversed() where ModFiles.isFile(file) && (file.lastPathComponent.hasSuffix(".vanilla.bak.tmp") || (file.lastPathComponent.hasPrefix(".mod_apply_") && file.lastPathComponent.hasSuffix(".tmp"))) { try ModFiles.files.removeItem(at: file) }
        let journal = target.appendingPathComponent(journalName)
        if ModFiles.exists(journal) {
            for line in try String(contentsOf: journal, encoding: .utf8).components(separatedBy: .newlines) {
                let relative = line.trimmingCharacters(in: .whitespacesAndNewlines).replacingOccurrences(of: "\\", with: "/")
                if relative.isEmpty || relative.hasPrefix("/") || relative.contains("..") { continue }
                guard let file = try? ModFiles.checked(target, relative) else { continue }
                if (!ModFiles.exists(file) || ModFiles.isFile(file)) && !ModFiles.isFile(file.appendingPathExtension("vanilla.bak")) {
                    try ModFiles.removeIfPresent(file); try pruneEmptyParentDirs(file.deletingLastPathComponent(), stop: target)
                }
            }
            try ModFiles.files.removeItem(at: journal)
        }
        let asra = target.appendingPathComponent("AsraTest.ini")
        if ModFiles.isFile(asra) && !ModFiles.isFile(asra.appendingPathExtension("vanilla.bak")) { try ModFiles.files.removeItem(at: asra) }
        for backup in try ModFiles.walk(target).reversed() where ModFiles.isFile(backup) && backup.lastPathComponent.hasSuffix(".vanilla.bak") {
            let original = backup.deletingLastPathComponent().appendingPathComponent(String(backup.lastPathComponent.dropLast(".vanilla.bak".count)))
            try ModFiles.replace(backup, original)
        }
    }
    @discardableResult
    static func applyModStaging(_ staging: URL, to target: URL, cleanReplace: Bool = true, isCnPatch: Bool = false, allowEvenicle2IncrementalDetect: Bool = false, commit: (Bool) throws -> Void = { _ in }) throws -> Bool {
        if isCnPatch && ModGameDescriptor.classifyDirectory(target)?.id == "evenicle2" { try normalizeEvenicle2CnPatchStaging(staging) }
        let root = try findModRootDir(staging, isCnPatch: isCnPatch)
        var affected: Set<URL> = [target.appendingPathComponent(journalName), target.appendingPathComponent(journalName + ".tmp"), target.appendingPathComponent(InstalledModInfo.metaFileName), target.appendingPathComponent("AsraTest.ini")]
        var reserved: Set<URL> = [target.appendingPathComponent(journalName), target.appendingPathComponent(journalName + ".tmp"), target.appendingPathComponent(InstalledModInfo.metaFileName)]
        let before = try ModFiles.walk(target)
        let directories = before.filter(ModFiles.isDirectory).map { $0.canonicalFileURL }
        let directoryPaths = Set(directories.map(\.path))
        let originalFiles = Set(before.filter(ModFiles.isFile).map { $0.canonicalFileURL.path })
        for file in try ModFiles.walk(root) where ModFiles.isFile(file) {
            let relative = try ModFiles.relative(file, to: root)
            if relative.hasPrefix(".") || NativeGameImport.skip(relative) { continue }
            let destination = try resolveCaseInsensitiveDest(target, relative)
            let artifacts = [destination.appendingPathExtension("vanilla.bak"), destination.appendingPathExtension("vanilla.bak.tmp"), destination.deletingLastPathComponent().appendingPathComponent(".mod_apply_" + destination.lastPathComponent + ".tmp")]
            affected.formUnion([destination] + artifacts)
            reserved.formUnion(artifacts)
        }
        for file in reserved where ModFiles.exists(file) && !ModFiles.isFile(file) {
            throw ModError.message("MOD 备份或临时路径不是文件：" + file.lastPathComponent)
        }
        if cleanReplace {
            for file in before where ModFiles.isFile(file) && (file.lastPathComponent.hasSuffix(".vanilla.bak.tmp") || (file.lastPathComponent.hasPrefix(".mod_apply_") && file.lastPathComponent.hasSuffix(".tmp"))) {
                affected.insert(file)
            }
            for file in before where ModFiles.isFile(file) && file.lastPathComponent.hasSuffix(".vanilla.bak") {
                affected.insert(file)
                affected.insert(file.deletingLastPathComponent().appendingPathComponent(String(file.lastPathComponent.dropLast(".vanilla.bak".count))))
            }
            let journal = target.appendingPathComponent(journalName)
            if ModFiles.isFile(journal) {
                for line in try String(contentsOf: journal, encoding: .utf8).components(separatedBy: .newlines) where !line.isEmpty {
                    affected.insert(try ModFiles.checked(target, line))
                }
            }
        }
        affected = Set(affected.map { $0.canonicalFileURL })
        let rollback = target.deletingLastPathComponent().appendingPathComponent(".mod_rollback_" + UUID().uuidString)
        try ModFiles.files.createDirectory(at: rollback, withIntermediateDirectories: true)
        var keepRollback = false
        defer { if !keepRollback { try? ModFiles.removeIfPresent(rollback) } }
        var saved: [URL: URL] = [:]
        for file in affected.sorted(by: { $0.path < $1.path }) where ModFiles.isFile(file) {
            let copy = rollback.appendingPathComponent(try ModFiles.relative(file, to: target))
            try ModFiles.files.createDirectory(at: copy.deletingLastPathComponent(), withIntermediateDirectories: true)
            try ModFiles.files.copyItem(at: file, to: copy)
            saved[file] = copy
        }
        do {
            let full = try applyStaging(staging, to: target, cleanReplace: cleanReplace, isCnPatch: isCnPatch, allowEvenicle2IncrementalDetect: allowEvenicle2IncrementalDetect)
            try commit(full)
            return full
        } catch {
            let failure = error
            do {
                for file in try ModFiles.walk(target) where ModFiles.isFile(file) && !originalFiles.contains(file.canonicalFileURL.path) {
                    try ModFiles.files.removeItem(at: file)
                }
                for directory in try ModFiles.walk(target).reversed() where ModFiles.isDirectory(directory) && !directoryPaths.contains(directory.canonicalFileURL.path) && ModFiles.children(directory).isEmpty {
                    try ModFiles.files.removeItem(at: directory)
                }
                for directory in directories where !ModFiles.exists(directory) {
                    try ModFiles.files.createDirectory(at: directory, withIntermediateDirectories: true)
                }
                for (file, copy) in saved {
                    try ModFiles.removeIfPresent(file)
                    try ModFiles.files.createDirectory(at: file.deletingLastPathComponent(), withIntermediateDirectories: true)
                    try ModFiles.replace(copy, file)
                }
            } catch {
                keepRollback = true
                throw ModError.message("安装失败，恢复文件保留在：" + rollback.path)
            }
            throw failure
        }
    }

    private static func applyStaging(_ staging: URL, to target: URL, cleanReplace: Bool, isCnPatch: Bool, allowEvenicle2IncrementalDetect: Bool) throws -> Bool {
        let evenicle2 = isCnPatch && ModGameDescriptor.classifyDirectory(target)?.id == "evenicle2"
        if evenicle2 { try normalizeEvenicle2CnPatchStaging(staging) }
        let root = try findModRootDir(staging, isCnPatch: isCnPatch)
        let full = isStandaloneFullModDir(root)
        let files = try ModFiles.walk(root).filter(ModFiles.isFile)
        let clean = evenicle2 && cleanReplace && allowEvenicle2IncrementalDetect ? files.contains(where: { $0.lastPathComponent.lowercased() == "evenicle2cg.afa" }) : cleanReplace
        if clean && (detectInstalledMod(target) || canRestoreVanilla(target)) { try restoreVanilla(target) }
        let journal = target.appendingPathComponent(journalName)
        var added = ((try? String(contentsOf: journal, encoding: .utf8)) ?? "").components(separatedBy: .newlines).map { $0.trimmingCharacters(in: .whitespacesAndNewlines) }.filter { !$0.isEmpty && !$0.contains("..") }
        var addedNames = Set(added.map { $0.lowercased() })
        var persisted: [String]?
        func persist() throws {
            if persisted != added { try ModFiles.journal(added, to: journal); persisted = added }
        }
        let accepted = try files.compactMap { file -> (URL, String)? in
            let relative = try ModFiles.relative(file, to: root)
            if relative.isEmpty || relative.hasPrefix(".") || relative.contains("..") || relative.hasSuffix(".vanilla.bak") || relative.hasSuffix(".vanilla.bak.tmp") || NativeGameImport.skip(relative) || (evenicle2 && isEvenicle2CnPatchJunkRelPath(relative)) { return nil }
            return (file, relative)
        }
        do {
            for (_, relative) in accepted {
                let destination = try resolveCaseInsensitiveDest(target, relative)
                guard !ModFiles.isDirectory(destination) else { throw ModError.message("MOD 目标路径不是文件：" + relative) }
                try ModFiles.files.createDirectory(at: destination.deletingLastPathComponent(), withIntermediateDirectories: true)
                let resolved = try ModFiles.relative(destination, to: target)
                if !ModFiles.exists(destination) && addedNames.insert(resolved.lowercased()).inserted { added.append(resolved) }
            }
            if !added.isEmpty || ModFiles.exists(journal) { try persist() }
            for (source, relative) in accepted {
                let destination = try resolveCaseInsensitiveDest(target, relative)
                let resolved = try ModFiles.relative(destination, to: target)
                if ModFiles.exists(destination) && !addedNames.contains(resolved.lowercased()) {
                    let backup = destination.appendingPathExtension("vanilla.bak")
                    if !ModFiles.exists(backup) {
                        let temporary = destination.appendingPathExtension("vanilla.bak.tmp")
                        try ModFiles.removeIfPresent(temporary)
                        do { try ModFiles.files.copyItem(at: destination, to: temporary); try ModFiles.replace(temporary, backup) }
                        catch { try? ModFiles.removeIfPresent(temporary); throw error }
                    }
                }
                let temporary = destination.deletingLastPathComponent().appendingPathComponent(".mod_apply_" + destination.lastPathComponent + ".tmp")
                try ModFiles.removeIfPresent(temporary)
                try ModFiles.replace(source, temporary)
                try ModFiles.replace(temporary, destination)
            }
            if ModFiles.isFile(target.appendingPathComponent("Rance10.ain")) && !ModFiles.isFile(target.appendingPathComponent("AsraTest.ini.vanilla.bak")) && !added.contains("AsraTest.ini") { added.append("AsraTest.ini") }
        } catch {
            if !added.isEmpty || ModFiles.exists(journal) { try? persist() }
            throw error
        }
        if !added.isEmpty || ModFiles.exists(journal) { try persist() }
        return clean || full
    }
}
