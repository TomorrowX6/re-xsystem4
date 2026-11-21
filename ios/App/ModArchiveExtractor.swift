import Foundation
import CoreFoundation

enum ArchiveExtractionError: LocalizedError {
    case unsafePath(String), native(String)
    var errorDescription: String? {
        switch self {
        case .unsafePath(let path): return "Archive entry escapes destination: \(path)"
        case .native(let message): return message
        }
    }
}

enum NativeGameImport {
    static func skip(_ path: String) -> Bool {
        let parts = path.replacingOccurrences(of: "\\", with: "/").split(separator: "/").map(String.init)
        guard let leaf = parts.last else { return false }
        let excluded = ["android-port", "savedata", "directx"]
        if parts.dropLast().contains(where: { $0.hasPrefix(".") || excluded.contains($0.lowercased()) }) { return true }
        let lower = leaf.lowercased()
        if (leaf.hasPrefix(".") && lower != ".xsys35rc") || excluded.contains(lower) { return true }
        if ["opensavefolder.exe", "resetconfig.exe", "uninstaller.exe"].contains(lower) { return true }
        return ["dll", "lnk", "bat", "cmd", "ps1", "mds"].contains((lower as NSString).pathExtension)
    }
}

enum ModArchiveExtractor {
    private static let gbk = String.Encoding(rawValue: CFStringConvertEncodingToNSStringEncoding(CFStringEncoding(CFStringEncodings.GB_18030_2000.rawValue)))
    private static let sjis = String.Encoding.shiftJIS
    private static let cp932 = String.Encoding(rawValue: CFStringConvertEncodingToNSStringEncoding(CFStringConvertWindowsCodepageToEncoding(932)))

    private static func decodeLossy(_ data: Data, encoding: String) -> String {
        data.withUnsafeBytes { bytes in
            guard let decoded = ah_archive_decode(bytes.bindMemory(to: UInt8.self).baseAddress, data.count, encoding) else {
                return String(decoding: data, as: UTF8.self)
            }
            defer { free(decoded) }
            return String(cString: decoded)
        }
    }

    static func isSupportedArchiveName(_ name: String) -> Bool {
        [".zip", ".rar", ".7z", ".tar", ".tar.gz", ".tgz", ".tar.xz", ".txz", ".iso"].contains { name.lowercased().hasSuffix($0) }
    }

    private static func score(_ text: String) -> Int {
        text.unicodeScalars.reduce(0) { score, ch in
            switch ch.value {
            case 0xFFFD: return score - 25
            case 0x3041...0x309F, 0x30A1...0x30FF: return score + 4
            case 0xFF61...0xFF9F: return score - 6
            case 0xFF01...0xFF5E: return score + 2
            case 0x4E00...0x9FFF: return score + 1
            default: return score
            }
        }
    }

    static func decodeEntryPath(_ data: Data) -> String {
        let decoded: String
        if let utf8 = String(data: data, encoding: .utf8) { decoded = utf8 }
        else {
            let bytes = Array(data)
            let japanese = String(data: data, encoding: sjis) ?? decodeLossy(data, encoding: "SHIFT_JIS")
            let chinese = String(data: data, encoding: gbk) ?? decodeLossy(data, encoding: "GB18030")
            var bonus = 0, penalty = 0, i = 0
            while i < bytes.count {
                let a = bytes[i]
                if a < 0x80 { i += 1; continue }
                if i + 1 < bytes.count {
                    let b = bytes[i + 1]
                    if (0xB0...0xF7).contains(a) && (0xA1...0xFE).contains(b) { bonus += 3 }
                    else if (0x81...0x9F).contains(a) && b < 0xA1 { bonus -= 3 }
                    i += 2
                } else { bonus -= 10; i += 1 }
            }
            i = 0
            while i + 1 < bytes.count {
                let a = bytes[i], b = bytes[i + 1]
                if a < 0x80 { i += 1; continue }
                if (0xB0...0xDF).contains(a) && (0xA1...0xFE).contains(b) { penalty -= 6; i += 2 }
                else { i += (0xA1...0xDF).contains(a) ? 1 : 2 }
            }
            decoded = score(japanese) + penalty > score(chinese) + bonus ? japanese : chinese
        }
        let separator = decoded.contains("\\") && !decoded.contains("/") ? "\\" : "/"
        return decoded.replacingOccurrences(of: "\\", with: "/").components(separatedBy: "/").map { segment in
            guard segment.unicodeScalars.contains(where: { $0.value >= 128 }),
                  let encoded = segment.data(using: gbk, allowLossyConversion: false) else { return segment }
            let bytes = Array(encoded)
            var i = 0, pairs = 0
            while i < bytes.count {
                let a = bytes[i]
                if a < 0x80 { i += 1; continue }
                guard !(0xA1...0xDF).contains(a),
                      (0x81...0x9F).contains(a) || (0xE0...0xEF).contains(a), i + 1 < bytes.count else { return segment }
                let b = bytes[i + 1]
                guard (0x40...0x7E).contains(b) || (0x80...0xFC).contains(b), a < 0xA1 || b < 0xA1 else { return segment }
                pairs += 1; i += 2
            }
            guard pairs > 0, let repaired = String(data: encoded, encoding: cp932), !repaired.contains("\u{FFFD}") else { return segment }
            return repaired
        }.joined(separator: separator)
    }

    @discardableResult
    static func extract(file: URL, to destination: URL,
                        shouldSkip: @escaping (String) -> Bool = { _ in false },
                        progress: @escaping (Int64, String) -> Void = { _, _ in },
                        isCancelled: @escaping () -> Bool = { false }) throws -> Int {
        let state = ExtractionState(destination: destination.canonicalFileURL,
                                    shouldSkip: shouldSkip, progress: progress, isCancelled: isCancelled)
        try FileManager.default.createDirectory(at: state.root, withIntermediateDirectories: true)
        var message = [CChar](repeating: 0, count: 1024)
        let result = ah_archive_extract(file.path, { context, bytes, count, size in
            let state = Unmanaged<ExtractionState>.fromOpaque(context!).takeUnretainedValue()
            do {
                return try state.accept(ModArchiveExtractor.decodeEntryPath(Data(bytes: bytes!, count: count)), size: size)
            } catch { state.error = error; return nil }
        }, { context, total in
            let state = Unmanaged<ExtractionState>.fromOpaque(context!).takeUnretainedValue()
            if state.error != nil || state.isCancelled() { return 1 }
            state.progress(total, state.currentPath)
            return 0
        }, Unmanaged.passUnretained(state).toOpaque(), &message, message.count)
        if let error = state.error { throw error }
        if isCancelled() { throw CancellationError() }
        guard result >= 0 else { throw ArchiveExtractionError.native(String(cString: message)) }
        return Int(result)
    }
}

private final class ExtractionState {
    let root: URL
    let shouldSkip: (String) -> Bool
    let progress: (Int64, String) -> Void
    let isCancelled: () -> Bool
    var error: Error?
    var currentPath = ""

    init(destination: URL, shouldSkip: @escaping (String) -> Bool,
         progress: @escaping (Int64, String) -> Void, isCancelled: @escaping () -> Bool) {
        root = destination; self.shouldSkip = shouldSkip
        self.progress = progress; self.isCancelled = isCancelled
    }

    func accept(_ decoded: String, size: Int64) throws -> UnsafeMutablePointer<CChar>? {
        let normalized = decoded.replacingOccurrences(of: "\\", with: "/")
        let parts = normalized.split(separator: "/").filter { $0 != "." }
        guard !normalized.hasPrefix("/"), !normalized.contains("\0"),
              !parts.contains(".."), parts.first?.contains(":") != true else {
            throw ArchiveExtractionError.unsafePath(decoded)
        }
        guard !parts.isEmpty, !normalized.hasSuffix("/") else { return nil }
        let relative = parts.joined(separator: "/")
        if parts.contains("__MACOSX") || NativeGameImport.skip(relative) || shouldSkip(relative) { return nil }
        var componentURL = root
        for part in parts {
            componentURL.appendPathComponent(String(part))
            if (try? FileManager.default.destinationOfSymbolicLink(atPath: componentURL.path)) != nil {
                throw ArchiveExtractionError.unsafePath(decoded)
            }
        }
        let output = root.appendingPathComponent(relative).canonicalFileURL
        guard output.relativeFilePath(to: root) != nil else { throw ArchiveExtractionError.unsafePath(decoded) }
        try FileManager.default.createDirectory(at: output.deletingLastPathComponent(), withIntermediateDirectories: true)
        if size > 0,
           let free = try FileManager.default.attributesOfFileSystem(forPath: root.path)[.systemFreeSize] as? NSNumber,
           size > free.int64Value - 32 * 1024 * 1024 { throw ArchiveExtractionError.native("ENOSPC: Insufficient storage space") }
        currentPath = relative
        return strdup(output.path)
    }
}

extension URL {
    /// Resolves the existing ancestor before appending missing components; Foundation does not resolve
    /// ancestor symlinks consistently when the full path does not exist.
    var canonicalFileURL: URL {
        var ancestor = standardizedFileURL
        var missing: [String] = []
        while ancestor.path != "/" && !FileManager.default.fileExists(atPath: ancestor.path) {
            missing.append(ancestor.lastPathComponent)
            ancestor.deleteLastPathComponent()
        }
        var resolved = ancestor.resolvingSymlinksInPath().standardizedFileURL
        for component in missing.reversed() { resolved.appendPathComponent(component) }
        return resolved
    }

    func relativeFilePath(to root: URL) -> String? {
        let path = canonicalFileURL.pathComponents
        let base = root.canonicalFileURL.pathComponents
        guard path.count > base.count, path.starts(with: base) else { return nil }
        return path.dropFirst(base.count).joined(separator: "/")
    }
}
