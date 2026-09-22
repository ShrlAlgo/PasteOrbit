import Foundation
import CryptoKit

struct Pairing: Codable {
    var version: Int
    var name: String
    var endpoint: String
    var key: String
}
struct SharedFile: Codable {
    var name: String
    var data: Data
    var localURL: URL? = nil
    private enum CodingKeys: String, CodingKey { case name, data }
}
struct TransferPart: Codable { var name: String; var length: Int64 }
struct TransferManifest: Codable { var kind: String; var parts: [TransferPart] }
struct Message: Codable {
    var id = UUID().uuidString.lowercased()
    var time = Int64(Date().timeIntervalSince1970)
    var kind = "text"
    var text = ""
    var files = [SharedFile]()
    var deviceId = ""
    var name = ""
    var replyTo = ""
}
private struct Envelope: Codable { var nonce: Data; var data: Data; var tag: Data }
final class TransferCancellation: @unchecked Sendable {
    private let lock = NSLock()
    private var stopped = false
    func cancel() { lock.lock(); stopped = true; lock.unlock() }
    var isCancelled: Bool { lock.lock(); defer { lock.unlock() }; return stopped }
}
enum Transfer {
    static let chunkSize = 256 * 1024
    static let aad = Data("PasteOrbit.Direct/1".utf8)
    static func fail(_ text: String) -> NSError { NSError(domain: "PasteOrbit", code: 1, userInfo: [NSLocalizedDescriptionKey: text]) }
    static func pairing(_ text: String) throws -> Pairing {
        let value = try JSONDecoder().decode(Pairing.self, from: Data(text.utf8))
        guard value.version == 1, !value.name.isEmpty, value.name.count <= 80,
              Data(base64Encoded: value.key)?.count == 32 else { throw fail("配对码无效") }
        _ = try endpoint(value)
        return value
    }
    static func endpoint(_ peer: Pairing) throws -> URL {
        guard let url = URLComponents(string: peer.endpoint), url.scheme == "http",
              url.path == "" || url.path == "/", url.query == nil, url.fragment == nil,
              url.user == nil, url.password == nil, let host = url.host else { throw fail("仅支持局域网 IPv4") }
        let parts = host.split(separator: ".", omittingEmptySubsequences: false)
        let bytes = parts.compactMap { UInt8($0) }
        guard parts.count == 4, bytes.count == 4,
              bytes[0] == 10 || bytes[0] == 127 || (bytes[0] == 192 && bytes[1] == 168)
                || (bytes[0] == 172 && (16...31).contains(bytes[1])) || (bytes[0] == 169 && bytes[1] == 254),
              let result = URL(string: peer.endpoint)?.appendingPathComponent("v1/transfer") else { throw fail("仅支持局域网 IPv4") }
        return result
    }
    static func validate(_ message: Message) throws {
        guard UUID(uuidString: message.id) != nil, message.files.count <= 32,
              ["text", "image", "files"].contains(message.kind),
              message.kind == "text" ? (!message.text.isEmpty && message.files.isEmpty) : !message.files.isEmpty,
              message.kind != "image" || message.files.count == 1 else { throw fail("内容无效，每次最多 32 个文件") }
        for file in message.files {
            guard !file.name.isEmpty, file.name.count <= 180, file.name != ".", file.name != "..",
                  !file.name.hasSuffix("."), !file.name.hasSuffix(" "),
                  file.name.rangeOfCharacter(from: .controlCharacters) == nil,
                  file.name.rangeOfCharacter(from: CharacterSet(charactersIn: "\\/:*?\"<>|")) == nil else { throw fail("文件名无效") }
        }
    }
    static func send(_ peer: Pairing, _ message: Message) async throws -> Message {
        if ["text", "image", "files"].contains(message.kind) { return try await upload(peer, message) }
        return try await sendRaw(peer, message)
    }
    static func sendRaw(_ peer: Pairing, _ message: Message) async throws -> Message {
        try Task.checkCancellation()
        guard let keyData = Data(base64Encoded: peer.key), keyData.count == 32 else { throw fail("密钥无效") }
        let key = SymmetricKey(data: keyData)
        let plain = try JSONEncoder().encode(message)
        guard plain.count <= 12 * 1024 * 1024 else { throw fail("消息过大") }
        let sealed = try AES.GCM.seal(plain, using: key, authenticating: aad)
        let envelope = Envelope(nonce: sealed.nonce.withUnsafeBytes { Data($0) }, data: sealed.ciphertext, tag: sealed.tag)
        var request = URLRequest(url: try endpoint(peer), timeoutInterval: 30)
        request.httpMethod = "POST"
        request.setValue("application/json", forHTTPHeaderField: "Content-Type")
        request.httpBody = try JSONEncoder().encode(envelope)
        let wire = try await BoundedRequest().run(request)
        let result = try JSONDecoder().decode(Envelope.self, from: wire)
        let box = try AES.GCM.SealedBox(nonce: AES.GCM.Nonce(data: result.nonce), ciphertext: result.data, tag: result.tag)
        let reply = try JSONDecoder().decode(Message.self, from: AES.GCM.open(box, using: key, authenticating: aad))
        guard UUID(uuidString: reply.id) != nil, reply.replyTo == message.id,
              abs(Double(reply.time) - Date().timeIntervalSince1970) <= 300 else { throw fail("响应无效，请校准设备时间") }
        if reply.kind == "error" { throw fail(reply.text) }
        return reply
    }

    // 明文只逐块进入加密器，每个请求仍使用已有的认证及超时保护。
    static func upload(_ peer: Pairing, _ message: Message) async throws -> Message {
        try validate(message)
        var sources = message.files
        var textURL: URL?
        defer { if let textURL { try? FileManager.default.removeItem(at: textURL) } }
        if message.kind == "text" {
            let url = try DeviceStore.stagingURL(); textURL = url
            FileManager.default.createFile(atPath: url.path, contents: nil, attributes: [.protectionKey: FileProtectionType.complete])
            let output = try FileHandle(forWritingTo: url)
            do {
                var buffer = Data(); buffer.reserveCapacity(chunkSize)
                for byte in message.text.utf8 {
                    buffer.append(byte)
                    if buffer.count == chunkSize { try Task.checkCancellation(); try DeviceStore.checkSpace(url, Int64(buffer.count)); try output.write(contentsOf: buffer); buffer.removeAll(keepingCapacity: true) }
                }
                try DeviceStore.checkSpace(url, Int64(buffer.count)); try output.write(contentsOf: buffer); try output.close()
            } catch { try? output.close(); throw error }
            sources = [SharedFile(name: "text.txt", data: Data(), localURL: url)]
        }
        let parts = try sources.map { file -> TransferPart in
            let length: Int64
            if let url = file.localURL { length = try DeviceStore.fileLength(url) } else { length = Int64(file.data.count) }
            return TransferPart(name: file.name, length: length)
        }
        let manifest = TransferManifest(kind: message.kind, parts: parts)
        var request = Message(); request.kind = "begin"; request.text = String(decoding: try JSONEncoder().encode(manifest), as: UTF8.self)
        let begin = try await sendRaw(peer, request)
        guard begin.kind == "ok", UUID(uuidString: begin.deviceId) != nil else { throw fail("请更新电脑以支持分块传输") }
        do {
            for (index, file) in sources.enumerated() {
                let input = try file.localURL.map { try FileHandle(forReadingFrom: $0) }
                defer { try? input?.close() }
                var offset: Int64 = 0
                while offset < parts[index].length {
                    try Task.checkCancellation()
                    let count = Int(min(Int64(chunkSize), parts[index].length - offset))
                    let data: Data
                    if let input { data = try input.read(upToCount: count) ?? Data() }
                    else { data = file.data.subdata(in: Int(offset)..<(Int(offset) + count)) }
                    guard !data.isEmpty else { throw fail("源文件已变化") }
                    var part = Message(); part.kind = "part"; part.deviceId = begin.deviceId; part.text = "\(index):\(offset)"
                    part.files = [SharedFile(name: "chunk", data: data)]
                    _ = try await sendRaw(peer, part); offset += Int64(data.count)
                }
                if let input, !(try input.read(upToCount: 1) ?? Data()).isEmpty { throw fail("源文件已变化") }
            }
            var commit = Message(); commit.kind = "commit"; commit.deviceId = begin.deviceId
            return try await sendRaw(peer, commit)
        } catch {
            var abort = Message(); abort.kind = "abort"; abort.deviceId = begin.deviceId
            let cleanup = abort
            _ = try? await Task.detached { try await sendRaw(peer, cleanup) }.value
            throw error
        }
    }
}

// 限制响应累计大小并拒绝重定向，避免局域网异常服务造成无限内存增长。
private final class BoundedRequest: NSObject, URLSessionDataDelegate {
    private var continuation: CheckedContinuation<Data, Error>?
    private var data = Data()
    private var session: URLSession?
    private let lock = NSLock()
    private var stopped = false
    private var requestTask: URLSessionDataTask?
    func run(_ request: URLRequest) async throws -> Data {
        try await withTaskCancellationHandler(operation: {
          try await withCheckedThrowingContinuation { continuation in
            self.continuation = continuation
            let config = URLSessionConfiguration.ephemeral
            config.connectionProxyDictionary = [:]
            config.timeoutIntervalForResource = 30
            session = URLSession(configuration: config, delegate: self, delegateQueue: nil)
            let task = session!.dataTask(with: request)
            lock.lock(); requestTask = task; let cancelled = stopped; lock.unlock()
            task.resume()
            if cancelled { task.cancel() }
          }
        }, onCancel: {
            self.lock.lock(); self.stopped = true; let task = self.requestTask; self.lock.unlock()
            task?.cancel()
        })
    }
    func urlSession(_ session: URLSession, task: URLSessionTask, willPerformHTTPRedirection response: HTTPURLResponse,
                    newRequest request: URLRequest, completionHandler: @escaping (URLRequest?) -> Void) { completionHandler(nil) }
    func urlSession(_ session: URLSession, dataTask: URLSessionDataTask, didReceive response: URLResponse,
                    completionHandler: @escaping (URLSession.ResponseDisposition) -> Void) {
        guard (response as? HTTPURLResponse)?.statusCode == 200, response.expectedContentLength <= 16 * 1024 * 1024 else {
            completionHandler(.cancel); return
        }
        completionHandler(.allow)
    }
    func urlSession(_ session: URLSession, dataTask: URLSessionDataTask, didReceive chunk: Data) {
        guard data.count + chunk.count <= 16 * 1024 * 1024 else { dataTask.cancel(); return }
        data.append(chunk)
    }
    func urlSession(_ session: URLSession, task: URLSessionTask, didCompleteWithError error: Error?) {
        if let error { continuation?.resume(throwing: error) } else { continuation?.resume(returning: data) }
        continuation = nil
        lock.lock(); requestTask = nil; lock.unlock()
        session.finishTasksAndInvalidate()
        self.session = nil
    }
}
