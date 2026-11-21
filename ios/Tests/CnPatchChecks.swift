import Foundation
import CryptoKit

@main
struct CnPatchChecks {
    static func hash(_ file: URL) throws -> String {
        let input = try FileHandle(forReadingFrom: file)
        defer { try? input.close() }
        var hash = SHA256()
        while let data = try input.read(upToCount: 1048576), !data.isEmpty { hash.update(data: data) }
        return hash.finalize().map { String(format: "%02x", $0) }.joined()
    }

    static func main() throws {
        let archive = URL(fileURLWithPath: CommandLine.arguments[1])
        let work = URL(fileURLWithPath: CommandLine.arguments[2])
        let fm = FileManager.default
        let archiveHash = try hash(archive)
        precondition(archiveHash == "b8d4ce1990245982195e6bc2a287368fd4a07d9b928296d4ee44b721fefae5ad")
        print("PASS: real V1.02 ZIP SHA-256 matches CDN catalog (562251373 bytes)")
        for variant in ["plain", "symlink"] {
            let base = work.appendingPathComponent(variant)
            try fm.createDirectory(at: base.appendingPathComponent("var/mobile"), withIntermediateDirectories: true)
            try fm.createDirectory(at: base.appendingPathComponent("private"), withIntermediateDirectories: true)
            try fm.createSymbolicLink(atPath: base.appendingPathComponent("private/var").path, withDestinationPath: "../var")
            let games = base.appendingPathComponent(variant == "symlink" ? "private/var/mobile/Containers/Data/Application/UUID/Documents/Games" : "var/mobile/Containers/Data/Application/UUID/Documents/Games")
            let game = games.appendingPathComponent("Evenicle2")
            let staging = games.appendingPathComponent(".mod_online_extract_temp_fixture")
            try fm.createDirectory(at: game, withIntermediateDirectories: true)
            try Data("vanilla".utf8).write(to: game.appendingPathComponent("Evenicle2.ain"))
            try Data("GameName = \"Evenicle2\"\n".utf8).write(to: game.appendingPathComponent("System40.ini"))
            let before = try snapshot(game)
            try ModArchiveExtractor.extract(file: archive, to: staging)
            let extracted = try snapshot(staging)
            print("Extracted \(variant): \(ModFiles.children(staging).map(\.lastPathComponent).sorted())")
            print("Root: \(game.canonicalFileURL.path)")
            let mai = game.appendingPathComponent("mai")
            print("Missing mai: \(mai.canonicalFileURL.path), relative: \(try ModFiles.relative(mai, to: game))")
            fflush(stdout)
            try ModOverlay.applyModStaging(staging, to: game, cleanReplace: true, isCnPatch: true) { _ in
                try InstalledModInfo(version: "V1.02", fileId: 1, source: "online", kind: "cn_patch").write(to: game)
            }
            for file in ["Evenicle2.ain", "Evenicle2EX.ex", "Evenicle2CG.afa", "Evenicle2Flat.afa", "mai/word.mai"] {
                precondition(ModFiles.isFile(game.appendingPathComponent(file)), file)
            }
            for (file, expected) in extracted {
                let installed = try hash(game.appendingPathComponent(file))
                precondition(installed == expected, file)
            }
            precondition(InstalledModInfo.read(game)?.version == "V1.02")
            try ModOverlay.restoreVanilla(game)
            let after = try snapshot(game)
            precondition(after == before && !ModFiles.exists(game.appendingPathComponent("mai")))
            print("PASS: real V1.02 extract → normalize → online apply → byte-identical restore (\(variant))")
        }
    }

    static func snapshot(_ directory: URL) throws -> [String: String] {
        var result: [String: String] = [:]
        for file in try ModFiles.walk(directory) where ModFiles.isFile(file) {
            result[try ModFiles.relative(file, to: directory)] = try hash(file)
        }
        return result
    }
}
