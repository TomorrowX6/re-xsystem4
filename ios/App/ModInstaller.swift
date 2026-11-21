import Foundation
import Combine
import CryptoKit

final class ModInstaller: ObservableObject {
    @Published private(set) var isRunning = false
    @Published private(set) var progressText = ""
    @Published private(set) var fraction: Double?
    @Published private(set) var succeeded = false
    @Published private(set) var failure: String?
    private let queue = DispatchQueue(label: "io.github.alicehub.mod-install", qos: .userInitiated)
    private let lock = NSLock()
    private var running = false
    private var cancelled = false
    private var networkTask: URLSessionTask?

    func cancel() {
        lock.lock(); cancelled = true; let task = networkTask; lock.unlock()
        task?.cancel()
    }
    private func isCancelled() -> Bool { lock.lock(); defer { lock.unlock() }; return cancelled }
    private func checkCancelled() throws { if isCancelled() { throw CancellationError() } }
    private func report(_ text: String, _ fraction: Double? = nil) {
        DispatchQueue.main.async { self.progressText = text; self.fraction = fraction.map { min(1, max(0, $0)) } }
    }
    @discardableResult
    private func start(_ work: @escaping () throws -> Void) -> Bool {
        lock.lock()
        guard !running else { lock.unlock(); return false }
        running = true; cancelled = false; lock.unlock()
        let publishStart = { self.isRunning = true; self.succeeded = false; self.failure = nil; self.progressText = "正在加载…"; self.fraction = nil }
        if Thread.isMainThread { publishStart() } else { DispatchQueue.main.async(execute: publishStart) }
        queue.async {
            var failure: Error?
            do { try self.checkCancelled(); try work() } catch { failure = error }
            let result = failure
            DispatchQueue.main.async {
                self.isRunning = false; self.succeeded = result == nil
                self.failure = result is CancellationError ? nil : result?.localizedDescription
                if result is CancellationError { self.progressText = "已取消" }
                else if result == nil { self.fraction = 1 }
                self.lock.lock(); self.running = false; self.networkTask = nil; self.lock.unlock()
            }
        }
        return true
    }

    @discardableResult
    func installOnlinePlan(_ steps: [ModReleaseEntry], to game: URL, descriptor: ModGameDescriptor) -> Bool {
        guard !steps.isEmpty else { return false }
        return start {
            for (index, release) in steps.enumerated() {
                try self.checkCancelled()
                let prefix = steps.count > 1 ? "[\(index + 1)/\(steps.count)] " : ""
                self.report(prefix + "准备下载 \(release.version)…", 0)
                let archive = try self.download(release, games: game.deletingLastPathComponent(), prefix: prefix)
                let staging = game.deletingLastPathComponent().appendingPathComponent(".mod_online_extract_temp_" + UUID().uuidString)
                defer { try? ModFiles.removeIfPresent(staging) }
                try self.extract(archive, to: staging)
                try self.checkCancelled()
                let previous = InstalledModInfo.read(game)
                try ModOverlay.applyModStaging(staging, to: game, cleanReplace: !release.isPatch, isCnPatch: descriptor.isCnPatch) { full in
                    try InstalledModInfo(version: release.version,
                    baseVersion: !release.isPatch || full ? release.version : (release.baseVersion ?? previous?.baseVersion),
                    fileId: release.fileId, baseFileId: !release.isPatch || full ? release.fileId : (release.baseFileId ?? previous?.baseFileId ?? -1),
                    fileName: release.fileName, uploadedAt: release.uploadedAt, source: "online",
                        installedAtMs: Int64(Date().timeIntervalSince1970 * 1000), kind: descriptor.overlayKind).write(to: game)
                }
                try ModFiles.removeIfPresent(archive)
                try ModFiles.removeIfPresent(archive.appendingPathExtension("meta"))
            }
        }
    }

    @discardableResult
    func installLocalArchive(_ source: URL, to game: URL, descriptor: ModGameDescriptor, cleanReplace: Bool = false) -> Bool {
        start {
            let access = source.startAccessingSecurityScopedResource()
            defer { if access { source.stopAccessingSecurityScopedResource() } }
            let staging = game.deletingLastPathComponent().appendingPathComponent(".mod_extract_temp_" + UUID().uuidString)
            defer { try? ModFiles.removeIfPresent(staging) }
            try self.extract(source, to: staging)
            try self.applyLocal(staging, label: source.lastPathComponent, game: game, descriptor: descriptor, cleanReplace: cleanReplace)
        }
    }

    @discardableResult
    func installLocalDirectory(_ source: URL, to game: URL, descriptor: ModGameDescriptor, cleanReplace: Bool = false) -> Bool {
        start {
            let access = source.startAccessingSecurityScopedResource()
            defer { if access { source.stopAccessingSecurityScopedResource() } }
            let staging = game.deletingLastPathComponent().appendingPathComponent(".mod_import_temp_" + UUID().uuidString)
            defer { try? ModFiles.removeIfPresent(staging) }
            var coordinationError: NSError?, copyError: Error?
            NSFileCoordinator().coordinate(readingItemAt: source, options: .withoutChanges, error: &coordinationError) { readable in
                do {
                    try ModFiles.files.createDirectory(at: staging, withIntermediateDirectories: true)
                    var total: Int64 = 0
                    for file in try ModFiles.walk(readable) {
                        try self.checkCancelled()
                        let relative = try ModFiles.relative(file, to: readable)
                        if NativeGameImport.skip(relative) { continue }
                        let destination = try ModFiles.checked(staging, relative)
                        if ModFiles.isDirectory(file) { try ModFiles.files.createDirectory(at: destination, withIntermediateDirectories: true) }
                        else if ModFiles.isFile(file) {
                            try ModFiles.files.createDirectory(at: destination.deletingLastPathComponent(), withIntermediateDirectories: true)
                            try ModFiles.files.copyItem(at: file, to: destination)
                            total += Int64((try file.resourceValues(forKeys: [.fileSizeKey]).fileSize) ?? 0)
                            self.report("已复制 \(total / 1048576) MB\n\(relative)")
                        }
                    }
                } catch { copyError = error }
            }
            if let error = coordinationError ?? copyError { throw error }
            try self.applyLocal(staging, label: source.lastPathComponent, game: game, descriptor: descriptor, cleanReplace: cleanReplace)
        }
    }

    @discardableResult
    func restoreVanilla(_ game: URL, descriptor: ModGameDescriptor) -> Bool {
        start {
            guard ModOverlay.canRestoreVanilla(game) else { throw ModError.message(descriptor.isCnPatch ? "当前无原版备份，请删除游戏本体后重装原版。" : "整合包无原版备份，请删除游戏本体后重装原版。") }
            try self.checkCancelled()
            try ModOverlay.restoreVanilla(game)
            self.report(descriptor.isCnPatch ? "已卸载汉化补丁" : "已卸载 MOD", 1)
        }
    }

    private func applyLocal(_ staging: URL, label: String, game: URL, descriptor: ModGameDescriptor, cleanReplace: Bool) throws {
        try checkCancelled()
        let previous = InstalledModInfo.readOrInfer(game)
        try ModOverlay.applyModStaging(staging, to: game, cleanReplace: cleanReplace, isCnPatch: descriptor.isCnPatch) { full in
            let version = ModCatalog.inferLocalOverlayVersion(label, isPatchHint: !full, fallbackIniDir: full || descriptor.isCnPatch ? game : nil, isCnPatch: descriptor.isCnPatch)
            try InstalledModInfo(version: version, baseVersion: full ? version : previous?.baseVersion,
            baseFileId: full ? -1 : (previous?.baseFileId ?? -1), fileName: label, source: "local",
                installedAtMs: Int64(Date().timeIntervalSince1970 * 1000), kind: descriptor.overlayKind).write(to: game)
        }
    }
    private func extract(_ archive: URL, to staging: URL) throws {
        try ModArchiveExtractor.extract(file: archive, to: staging, progress: { bytes, path in self.report("已解压 \(bytes / 1048576) MB\n\(path)") }, isCancelled: { self.isCancelled() })
    }

    func downloadVerifiedFile(_ release: ModReleaseEntry, cache: URL,
                              progress: @escaping (Int64, Int64) -> Void) throws -> URL {
        try download(release, games: cache, prefix: "", cacheDirectory: cache, progress: progress)
    }

    private func download(_ release: ModReleaseEntry, games: URL, prefix: String,
                          cacheDirectory: URL? = nil, progress: ((Int64, Int64) -> Void)? = nil) throws -> URL {
        let cache = cacheDirectory ?? games.appendingPathComponent(".online_mod_cache")
        try ModFiles.files.createDirectory(at: cache, withIntermediateDirectories: true)
        let hash = release.expectedSha256 ?? ""
        guard hash.isEmpty || hash.modMatch(#"^([0-9a-f]{64})$"#) != nil else { throw ModError.message("SHA-256 校验值无效") }
        let identity = hash.count >= 16 ? String(hash.prefix(16)) : String(SHA256.hash(data: Data(release.downloadUrl.utf8)).map { String(format: "%02x", $0) }.joined().prefix(16))
        let ext = [".tar.gz", ".tar.xz", ".rar", ".7z", ".zip", ".tgz", ".txz"].first { release.fileName.lowercased().hasSuffix($0) } ?? ".rar"
        let part = cache.appendingPathComponent("mod_" + identity + ext + ".part")
        let meta = part.appendingPathExtension("meta")
        func metadata() -> [String: Any]? {
            guard let data = try? Data(contentsOf: meta), let json = try? JSONSerialization.jsonObject(with: data) as? [String: Any] else { return nil }
            if !hash.isEmpty && (json["sha256"] as? String)?.lowercased() == hash.lowercased() { return json }
            return json["url"] as? String == release.downloadUrl ? json : nil
        }
        var lastError: Error = ModError.message("下载失败，请检查网络与可用空间")
        for attempt in 0..<8 {
            try checkCancelled()
            let saved = metadata(), etag = saved?["etag"] as? String
            if (saved == nil && (ModFiles.exists(meta) || hash.isEmpty)) || (hash.isEmpty && !ModDownloadTransfer.strong(etag)) {
                try ModFiles.removeIfPresent(part); try ModFiles.removeIfPresent(meta)
            }
            let length = ModFiles.fileSize(part)
            let expected = release.expectedSize > 0 ? release.expectedSize : ((saved?["totalBytes"] as? NSNumber)?.int64Value ?? -1)
            if expected > 0 && length == expected {
                if hash.isEmpty { return part }
                if try fileHash(part) == hash.lowercased() { return part }
            }
            if expected > 0 && length >= expected { try ModFiles.removeIfPresent(part); try ModFiles.removeIfPresent(meta) }
            do {
                let transfer = ModDownloadTransfer(part: part, release: release, saved: metadata(), cancelled: { self.isCancelled() }, progress: { done, total in
                    if let progress { progress(done, total) }
                    else {
                        let fraction = total > 0 ? Double(done) / Double(total) : nil
                        self.report(prefix + (fraction.map { "下载中 · \(Int($0 * 100))% · -- MB/s\n\(done / 1048576) / \(max(1, total / 1048576)) MB · \(release.version)" } ?? "下载中 · \(done / 1048576) MB · -- MB/s\n\(release.version)"), fraction)
                    }
                })
                try transfer.run { task in self.lock.lock(); self.networkTask = task; let cancel = self.cancelled; self.lock.unlock(); if cancel { task.cancel() } }
                try checkCancelled()
                let downloaded = ModFiles.fileSize(part)
                if release.expectedSize > 0 && downloaded != release.expectedSize { throw ModError.message("下载文件大小不匹配") }
                let hashMatches = hash.isEmpty ? true : try fileHash(part) == hash.lowercased()
                if !hashMatches {
                    try ModFiles.removeIfPresent(part); try ModFiles.removeIfPresent(meta)
                    throw ModError.message("SHA-256 校验失败")
                }
                return part
            } catch {
                try checkCancelled(); lastError = error
                if attempt < 7 { for _ in 0..<(min(attempt + 1, 5) * 5) { try checkCancelled(); Thread.sleep(forTimeInterval: 0.2) } }
            }
        }
        throw lastError
    }
    private func fileHash(_ file: URL) throws -> String {
        let input = try FileHandle(forReadingFrom: file)
        defer { try? input.close() }
        var hash = SHA256()
        while true {
            try checkCancelled()
            let data = try input.read(upToCount: 1048576) ?? Data()
            if data.isEmpty { break }; hash.update(data: data)
        }
        return hash.finalize().map { String(format: "%02x", $0) }.joined()
    }
}

private final class ModDownloadTransfer: NSObject, URLSessionDataDelegate {
    let part: URL
    let release: ModReleaseEntry
    let saved: [String: Any]?
    let cancelled: () -> Bool
    let progress: (Int64, Int64) -> Void
    let done = DispatchSemaphore(value: 0)
    var handle: FileHandle?
    var error: Error?
    var received: Int64 = 0
    var total: Int64 = -1
    init(part: URL, release: ModReleaseEntry, saved: [String: Any]?, cancelled: @escaping () -> Bool, progress: @escaping (Int64, Int64) -> Void) {
        self.part = part; self.release = release; self.saved = saved; self.cancelled = cancelled; self.progress = progress
    }
    static func strong(_ etag: String?) -> Bool { etag.map { !$0.isEmpty && !$0.trimmingCharacters(in: .whitespaces).lowercased().hasPrefix("w/") } ?? false }
    func run(register: (URLSessionTask) -> Void) throws {
        guard let url = URL(string: release.downloadUrl), ["http", "https"].contains(url.scheme) else { throw ModError.message("无效下载地址") }
        var request = URLRequest(url: url, cachePolicy: .reloadIgnoringLocalCacheData, timeoutInterval: 60)
        request.setValue("AliceHub", forHTTPHeaderField: "User-Agent")
        request.setValue("identity", forHTTPHeaderField: "Accept-Encoding")
        received = ModFiles.fileSize(part)
        if received > 0 {
            request.setValue("bytes=\(received)-", forHTTPHeaderField: "Range")
            if let etag = saved?["etag"] as? String, Self.strong(etag) { request.setValue(etag, forHTTPHeaderField: "If-Range") }
        }
        let session = URLSession(configuration: .ephemeral, delegate: self, delegateQueue: nil)
        defer { session.invalidateAndCancel() }
        let task = session.dataTask(with: request)
        register(task); task.resume(); done.wait()
        if let error { throw error }
        if total > 0 && received != total { throw ModError.message("下载文件大小不匹配") }
    }
    func urlSession(_ session: URLSession, dataTask: URLSessionDataTask, didReceive response: URLResponse, completionHandler: @escaping (URLSession.ResponseDisposition) -> Void) {
        do {
            guard !cancelled(), let response = response as? HTTPURLResponse else { throw CancellationError() }
            let etag = response.value(forHTTPHeaderField: "ETag")
            if response.statusCode == 206 {
                let range = response.value(forHTTPHeaderField: "Content-Range") ?? ""
                guard let start = range.modMatch(#"^bytes (\d+)-\d+/\d+$"#), Int64(start) == received,
                      !(release.expectedSha256 ?? "").isEmpty || (received > 0 && Self.strong(etag) && etag == saved?["etag"] as? String) else {
                    try ModFiles.removeIfPresent(part); try ModFiles.removeIfPresent(part.appendingPathExtension("meta")); throw ModError.message("下载续传校验失败")
                }
                total = Int64(range.components(separatedBy: "/").last ?? "") ?? -1
            } else if response.statusCode == 200 {
                received = 0; total = response.expectedContentLength
            } else {
                if response.statusCode == 416 { try ModFiles.removeIfPresent(part); try ModFiles.removeIfPresent(part.appendingPathExtension("meta")) }
                throw ModError.message("下载失败 HTTP \(response.statusCode)")
            }
            if release.expectedSize >= 0 && total >= 0 && total != release.expectedSize {
                throw ModError.message("下载文件大小不匹配")
            }
            if total > received,
               let free = try ModFiles.files.attributesOfFileSystem(forPath: part.deletingLastPathComponent().path)[.systemFreeSize] as? NSNumber,
               total - received > free.int64Value - 64 * 1048576 { throw ModError.message("存储空间不足") }
            if !ModFiles.exists(part) { guard ModFiles.files.createFile(atPath: part.path, contents: nil) else { throw ModError.message("下载失败，请检查网络与可用空间") } }
            handle = try FileHandle(forWritingTo: part)
            if received == 0 { try handle!.truncate(atOffset: 0) }
            try handle!.seekToEnd()
            var json: [String: Any] = ["url": release.downloadUrl, "totalBytes": total]
            if let etag { json["etag"] = etag }
            if let sha = release.expectedSha256 { json["sha256"] = sha }
            try JSONSerialization.data(withJSONObject: json).write(to: part.appendingPathExtension("meta"), options: .atomic)
            completionHandler(.allow)
        } catch { self.error = error; completionHandler(.cancel) }
    }
    func urlSession(_ session: URLSession, dataTask: URLSessionDataTask, didReceive data: Data) {
        do {
            if cancelled() { throw CancellationError() }
            if release.expectedSize >= 0 && Int64(data.count) > release.expectedSize - received {
                throw ModError.message("下载文件大小不匹配")
            }
            try handle?.write(contentsOf: data); received += Int64(data.count); progress(received, total)
        } catch { self.error = error; dataTask.cancel() }
    }
    func urlSession(_ session: URLSession, task: URLSessionTask, didCompleteWithError error: Error?) {
        do { try handle?.synchronize(); try handle?.close() } catch { self.error = self.error ?? error }
        self.error = self.error ?? error
        done.signal()
    }
}
