import UIKit
import UniformTypeIdentifiers

final class ShareController: UIViewController {
    private let status = UILabel()
    private let sendButton = UIButton(type: .system)
    private var operation: Task<Void, Never>?
    private var staged = [URL]()
    override func viewDidLoad() {
        super.viewDidLoad()
        view.backgroundColor = .systemBackground
        status.numberOfLines = 0
        status.text = "发送到已配对电脑。请先在 PasteOrbit 主应用完成配对并允许访问本地网络。分块传输，不设总大小上限；每次最多 32 个文件。"
        sendButton.setTitle("发送", for: .normal)
        sendButton.addTarget(self, action: #selector(send), for: .touchUpInside)
        let cancel = UIButton(type: .system); cancel.setTitle("取消", for: .normal)
        cancel.addTarget(self, action: #selector(close), for: .touchUpInside)
        let stack = UIStackView(arrangedSubviews: [status, sendButton, cancel]); stack.axis = .vertical; stack.spacing = 20
        stack.translatesAutoresizingMaskIntoConstraints = false; view.addSubview(stack)
        NSLayoutConstraint.activate([stack.leadingAnchor.constraint(equalTo: view.leadingAnchor, constant: 24),
            stack.trailingAnchor.constraint(equalTo: view.trailingAnchor, constant: -24), stack.topAnchor.constraint(equalTo: view.safeAreaLayoutGuide.topAnchor, constant: 24)])
    }
    @objc private func close() { operation?.cancel(); extensionContext?.cancelRequest(withError: Transfer.fail("已取消")) }
    @objc private func send() {
        sendButton.isEnabled = false
        operation = Task { @MainActor in
            defer { for url in staged { try? FileManager.default.removeItem(at: url) }; staged.removeAll() }
            do {
                let peer = try DeviceStore.peer()
                let message = try await content()
                try Transfer.validate(message)
                _ = try await Transfer.send(peer, message)
                extensionContext?.completeRequest(returningItems: nil)
            } catch { status.text = error.localizedDescription; sendButton.isEnabled = true }
        }
    }
    private func content() async throws -> Message {
        let items = (extensionContext?.inputItems as? [NSExtensionItem]) ?? []
        let providers = items.flatMap { $0.attachments ?? [] }
        guard providers.count <= 32 else { throw Transfer.fail("每次最多 32 个附件") }
        var message = Message()
        var allImages = true
        for provider in providers {
            if provider.hasItemConformingToTypeIdentifier(UTType.fileURL.identifier) {
                let value = try await item(provider, UTType.fileURL.identifier)
                guard let url = value as? URL, url.isFileURL else { throw Transfer.fail("文件地址无效") }
                let scoped = url.startAccessingSecurityScopedResource()
                defer { if scoped { url.stopAccessingSecurityScopedResource() } }
                let staging = Task.detached { try DeviceStore.stage(url) }
                let local = try await withTaskCancellationHandler(operation: { try await staging.value }, onCancel: { staging.cancel() })
                staged.append(local); allImages = false
                message.files.append(SharedFile(name: url.lastPathComponent, data: Data(), localURL: local))
            } else if provider.hasItemConformingToTypeIdentifier(UTType.image.identifier)
                        || (provider.hasItemConformingToTypeIdentifier(UTType.data.identifier)
                            && !provider.hasItemConformingToTypeIdentifier(UTType.plainText.identifier)
                            && !provider.hasItemConformingToTypeIdentifier(UTType.url.identifier)) {
                let isImage = provider.hasItemConformingToTypeIdentifier(UTType.image.identifier)
                let type = isImage ? UTType.image.identifier : UTType.data.identifier
                let file = try await file(provider, type)
                if let local = file.localURL { staged.append(local) }
                message.files.append(file); allImages = allImages && isImage
            } else if provider.hasItemConformingToTypeIdentifier(UTType.url.identifier) {
                let value = try await item(provider, UTType.url.identifier)
                if let url = value as? URL {
                    guard !url.isFileURL else { throw Transfer.fail("此应用只提供文件地址，无法读取，请改从文件应用分享") }
                    message.text += (message.text.isEmpty ? "" : "\n") + url.absoluteString
                }
            } else if provider.hasItemConformingToTypeIdentifier(UTType.plainText.identifier) {
                let value = try await item(provider, UTType.plainText.identifier)
                if let text = value as? String { message.text += (message.text.isEmpty ? "" : "\n") + text }
            } else { throw Transfer.fail("不支持此分享类型") }
        }
        if providers.isEmpty { message.text = items.compactMap { $0.attributedContentText?.string }.joined(separator: "\n") }
        if !message.files.isEmpty { message.kind = allImages && message.files.count == 1 ? "image" : "files" }
        return message
    }
    private func item(_ provider: NSItemProvider, _ type: String) async throws -> NSSecureCoding? {
        try await withCheckedThrowingContinuation { continuation in
            provider.loadItem(forTypeIdentifier: type, options: nil) { value, error in
                if let error { continuation.resume(throwing: error) } else { continuation.resume(returning: value) }
            }
        }
    }
    private func file(_ provider: NSItemProvider, _ type: String) async throws -> SharedFile {
        let cancellation = TransferCancellation()
        return try await withTaskCancellationHandler(operation: {
          try await withCheckedThrowingContinuation { continuation in
            // 临时 URL 仅在回调内有效；读取前检查大小，不使用远端提供的目录。
            provider.loadFileRepresentation(forTypeIdentifier: type) { url, error in
                do {
                    if let error { throw error }
                    guard let url else { throw Transfer.fail("附件不可读") }
                    let local = try DeviceStore.stage(url, cancelled: { cancellation.isCancelled })
                    continuation.resume(returning: SharedFile(name: url.lastPathComponent, data: Data(), localURL: local))
                } catch { continuation.resume(throwing: error) }
            }
          }
        }, onCancel: { cancellation.cancel() })
    }
}
