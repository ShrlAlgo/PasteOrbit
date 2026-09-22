import SwiftUI
import UIKit
import ImageIO

@main struct PasteOrbitApp: App {
    var body: some Scene { WindowGroup { ContentView() } }
}
private struct ShareSheet: UIViewControllerRepresentable {
    let items: [Any]
    func makeUIViewController(context: Context) -> UIActivityViewController { UIActivityViewController(activityItems: items, applicationActivities: nil) }
    func updateUIViewController(_ uiViewController: UIActivityViewController, context: Context) {}
}
private struct ContentView: View {
    @State private var code = ""
    @State private var text = ""
    @State private var status = "先导入电脑配对码，并允许访问本地网络。"
    @State private var busy = false
    @State private var inbox = [URL]()
    @State private var selected: URL?
    @State private var shareItems = [Any]()
    @State private var sharing = false
    @State private var operation: Task<Void, Never>?
    var body: some View {
        NavigationStack {
            Form {
                Section("局域网直连 · 无中间服务器") {
                    Text("分块传输，不设总大小上限。电脑开启设备直连后才能发送或接收。")
                    TextField("电脑配对码（含密钥）", text: $code).textInputAutocapitalization(.never).autocorrectionDisabled()
                    Button("配对电脑") {
                        let value = code
                        work {
                            let peer = try Transfer.pairing(value)
                            var hello = Message(); hello.kind = "hello"; hello.deviceId = try DeviceStore.deviceId(); hello.name = UIDevice.current.name
                            _ = try await Transfer.send(peer, hello)
                            try DeviceStore.save(peer); code = ""
                            return "已配对：" + peer.name
                        }
                    }
                }
                Section("发送") {
                    TextEditor(text: $text).frame(minHeight: 80)
                    Button("发送文字") {
                        let value = text
                        work {
                            var message = Message(); message.text = value
                            try Transfer.validate(message)
                            _ = try await Transfer.send(DeviceStore.peer(), message)
                            return "已发送到电脑收件箱"
                        }
                    }
                    Text("图片、文件和链接：在其他应用点击分享 → PasteOrbit。")
                }
                Section("收件箱") {
                    Button("接收电脑发来的内容") { work { try await DeviceStore.receive(DeviceStore.peer()) } }
                    Picker("选择收件", selection: $selected) {
                        Text("请选择").tag(Optional<URL>.none)
                        ForEach(inbox, id: \.self) { url in
                            Text(url.deletingPathExtension().lastPathComponent).tag(Optional(url))
                        }
                    }
                    Button("复制文字或图片") { work {
                        guard let selected else { throw Transfer.fail("请选择收件") }
                        let message = try DeviceStore.read(selected)
                        if message.kind == "text" { UIPasteboard.general.string = message.text }
                        else if message.kind == "image" {
                            let file = message.files[0]
                            if let url = file.localURL, try DeviceStore.fileLength(url) > 8 * 1024 * 1024 {
                                throw Transfer.fail("原图已完整保存，请使用分享/保存到文件，避免整图加载占用过多内存")
                            }
                            let bytes = try file.localURL.map { try Data(contentsOf: $0) } ?? file.data
                            guard let source = CGImageSourceCreateWithData(bytes as CFData, nil),
                                  let info = CGImageSourceCopyPropertiesAtIndex(source, 0, nil) as? [CFString: Any],
                                  let width = info[kCGImagePropertyPixelWidth] as? NSNumber,
                                  let height = info[kCGImagePropertyPixelHeight] as? NSNumber,
                                  width.doubleValue * height.doubleValue <= 32_000_000,
                                  let image = UIImage(data: bytes) else { throw Transfer.fail("图片无效或像素过多，请使用分享保存文件") }
                            UIPasteboard.general.image = image
                        }
                        else { throw Transfer.fail("文件请使用分享/保存到文件") }
                        return "已写入剪贴板"
                    } }
                    Button("分享 / 保存到文件") { work {
                        guard let selected else { throw Transfer.fail("请选择收件") }
                        let message = try DeviceStore.read(selected)
                        if message.kind == "text" { shareItems = [message.text] }
                        else if message.files.allSatisfy({ $0.localURL != nil }) { shareItems = message.files.compactMap { $0.localURL } }
                        else {
                            let folder = FileManager.default.temporaryDirectory.appendingPathComponent("PasteOrbit-" + UUID().uuidString)
                            try FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)
                            shareItems = try message.files.enumerated().map { index, file -> URL in
                                if let local = file.localURL { return local }
                                let url = folder.appendingPathComponent("\(index + 1)-" + file.name)
                                try file.data.write(to: url, options: [.atomic, .completeFileProtection]); return url
                            }
                        }
                        sharing = true; return "请选择接收应用"
                    } }
                    Button("删除选中收件", role: .destructive) { work {
                        guard let selected else { throw Transfer.fail("请选择收件") }
                        let folder = selected.deletingPathExtension().appendingPathExtension("files")
                        try FileManager.default.removeItem(at: selected)
                        if FileManager.default.fileExists(atPath: folder.path) { try FileManager.default.removeItem(at: folder) }
                        self.selected = nil; return "已删除本机收件"
                    } }
                }
                Text(status)
            }.disabled(busy).navigationTitle("PasteOrbit")
                .toolbar { if busy { Button("取消") { operation?.cancel() } } }
                .onAppear { inbox = (try? DeviceStore.messages()) ?? [] }
                .sheet(isPresented: $sharing) { ShareSheet(items: shareItems) }
        }
    }
    @MainActor private func work(_ action: @escaping () async throws -> String) {
        guard !busy else { return }; busy = true
        operation = Task { @MainActor in
            defer { busy = false; inbox = (try? DeviceStore.messages()) ?? [] }
            do { status = try await action() } catch { status = error.localizedDescription }
        }
    }
}
