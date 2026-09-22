import Foundation

enum DeviceStore {
    static func fileLength(_ url: URL) throws -> Int64 {
        let attributes = try FileManager.default.attributesOfItem(atPath: url.path)
        guard attributes[.type] as? FileAttributeType == .typeRegular, let size = attributes[.size] as? NSNumber else { throw Transfer.fail("不是普通文件") }
        return size.int64Value
    }
    static func checkSpace(_ url: URL, _ bytes: Int64) throws {
        let attributes = try FileManager.default.attributesOfFileSystem(forPath: url.deletingLastPathComponent().path)
        guard let free = attributes[.systemFreeSize] as? NSNumber, bytes >= 0,
              free.int64Value - 64 * 1024 * 1024 >= bytes else { throw Transfer.fail("磁盘空间不足，内容仍保留在发送端") }
    }
    static func stagingURL() throws -> URL {
        let directory = FileManager.default.temporaryDirectory.appendingPathComponent("PasteOrbit-Staging", isDirectory: true)
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        return directory.appendingPathComponent(UUID().uuidString)
    }
    static func stage(_ source: URL, cancelled: () -> Bool = { false }) throws -> URL {
        let size = try fileLength(source), target = try stagingURL()
        try checkSpace(target, size)
        FileManager.default.createFile(atPath: target.path, contents: nil, attributes: [.protectionKey: FileProtectionType.complete])
        let input = try FileHandle(forReadingFrom: source)
        defer { try? input.close() }
        do {
            let output = try FileHandle(forWritingTo: target)
            defer { try? output.close() }
            while let bytes = try input.read(upToCount: Transfer.chunkSize), !bytes.isEmpty {
                if cancelled() { throw CancellationError() }
                try Task.checkCancellation(); try checkSpace(target, Int64(bytes.count)); try output.write(contentsOf: bytes)
            }
            try output.synchronize(); return target
        } catch { try? FileManager.default.removeItem(at: target); throw error }
    }
    static func root() throws -> URL {
        guard let url = FileManager.default.containerURL(forSecurityApplicationGroupIdentifier: "group.com.pasteorbit.mobile") else {
            throw Transfer.fail("App Group 未配置，请检查应用和分享扩展签名")
        }
        return url
    }
    static func peer() throws -> Pairing {
        try Transfer.pairing(String(contentsOf: root().appendingPathComponent("pairing.json"), encoding: .utf8))
    }
    static func save(_ peer: Pairing) throws {
        var url = try root().appendingPathComponent("pairing.json")
        try JSONEncoder().encode(peer).write(to: url, options: [.atomic, .completeFileProtection])
        var values = URLResourceValues(); values.isExcludedFromBackup = true
        try url.setResourceValues(values)
    }
    static func deviceId() throws -> String {
        let url = try root().appendingPathComponent("device-id")
        if let id = try? String(contentsOf: url, encoding: .utf8), UUID(uuidString: id) != nil { return id }
        let id = UUID().uuidString.lowercased()
        try Data(id.utf8).write(to: url, options: [.atomic, .completeFileProtection])
        return id
    }
    static func inbox() throws -> URL {
        let url = try root().appendingPathComponent("Inbox", isDirectory: true)
        try FileManager.default.createDirectory(at: url, withIntermediateDirectories: true)
        var protected = url
        var values = URLResourceValues(); values.isExcludedFromBackup = true
        try protected.setResourceValues(values)
        return url
    }
    static func messages() throws -> [URL] {
        try FileManager.default.contentsOfDirectory(at: inbox(), includingPropertiesForKeys: nil).filter { $0.pathExtension == "json" }.sorted { $0.lastPathComponent > $1.lastPathComponent }
    }
    // 先持久化再确认；重复轮询同一消息不会重复写入收件箱。
    static func receive(_ peer: Pairing) async throws -> String {
        var poll = Message(); poll.kind = "poll"; poll.deviceId = try deviceId()
        var message = try await Transfer.send(peer, poll)
        if message.kind == "empty" { return "暂无待收内容" }
        let bundle = message.kind == "bundle"
        if !bundle { try Transfer.validate(message) }
        let target = try inbox().appendingPathComponent(UUID(uuidString: message.id)!.uuidString + ".json")
        if !FileManager.default.fileExists(atPath: target.path) {
            guard try messages().count < 30 else { throw Transfer.fail("收件箱已满，请删除旧内容") }
            let folder = target.deletingPathExtension().appendingPathExtension("files")
            do {
                if bundle { message = try await download(peer, message, folder) }
                try JSONEncoder().encode(message).write(to: target, options: [.atomic, .completeFileProtection])
            } catch {
                if FileManager.default.fileExists(atPath: folder.path) { try? FileManager.default.removeItem(at: folder) }
                throw error
            }
        }
        var ack = Message(); ack.kind = "ack"; ack.deviceId = poll.deviceId; ack.text = message.id
        _ = try await Transfer.send(peer, ack)
        return "已接收，可在下方复制或分享"
    }
    static func read(_ url: URL) throws -> Message {
        var message = try JSONDecoder().decode(Message.self, from: Data(contentsOf: url))
        let folder = url.deletingPathExtension().appendingPathExtension("files")
        if FileManager.default.fileExists(atPath: folder.path) {
            for i in message.files.indices { message.files[i].localURL = folder.appendingPathComponent("\(i + 1)-" + message.files[i].name) }
            if message.kind == "text", let path = message.files.first?.localURL {
                if try fileLength(path) <= 8 * 1024 * 1024, let text = try? String(contentsOf: path, encoding: .utf8) {
                    message.text = text; message.files = []
                } else { message.kind = "files" }
            }
        }
        try Transfer.validate(message)
        return message
    }

    private static func download(_ peer: Pairing, _ bundle: Message, _ folder: URL) async throws -> Message {
        let manifest = try JSONDecoder().decode(TransferManifest.self, from: Data(bundle.text.utf8))
        guard ["text","image","files"].contains(manifest.kind), !manifest.parts.isEmpty, manifest.parts.count <= 32,
              manifest.kind == "files" || manifest.parts.count == 1 else { throw Transfer.fail("清单无效") }
        var stub = Message(); stub.kind = "files"; stub.files = manifest.parts.map { SharedFile(name: $0.name, data: Data()) }
        try Transfer.validate(stub)
        var total: Int64 = 0
        for part in manifest.parts {
            let sum = total.addingReportingOverflow(part.length)
            guard part.length >= 0, !sum.overflow else { throw Transfer.fail("长度无效") }; total = sum.partialValue
        }
        try checkSpace(folder, total)
        try FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)
        for (index, part) in manifest.parts.enumerated() {
            let url = folder.appendingPathComponent("\(index + 1)-" + part.name)
            FileManager.default.createFile(atPath: url.path, contents: nil, attributes: [.protectionKey: FileProtectionType.complete])
            let output = try FileHandle(forWritingTo: url); defer { try? output.close() }
            var offset: Int64 = 0, chunk: Int64 = 0
            while offset < part.length {
                try Task.checkCancellation()
                var request = Message(); request.kind = "read"; request.deviceId = bundle.deviceId; request.text = "\(index):\(chunk)"
                let reply = try await Transfer.send(peer, request)
                guard reply.kind == "chunk", reply.files.count == 1, let bytes = reply.files.first?.data,
                      !bytes.isEmpty, bytes.count <= Transfer.chunkSize, Int64(bytes.count) <= part.length - offset else { throw Transfer.fail("块长度无效") }
                try checkSpace(url, Int64(bytes.count)); try output.write(contentsOf: bytes)
                offset += Int64(bytes.count); chunk += 1
            }
            try output.synchronize()
        }
        var result = bundle; result.kind = manifest.kind; result.text = ""; result.files = stub.files
        return result
    }
}
