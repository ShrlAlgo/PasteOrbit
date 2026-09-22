package com.pasteorbit.mobile;

import android.app.*;
import android.os.*;
import android.content.*;
import android.net.Uri;
import android.database.Cursor;
import android.provider.OpenableColumns;
import android.security.keystore.*;
import android.view.View;
import android.widget.*;
import androidx.core.content.FileProvider;
import org.json.*;
import java.io.*;
import java.nio.charset.StandardCharsets;
import java.security.KeyStore;
import java.util.*;
import java.util.concurrent.*;
import javax.crypto.*;
import javax.crypto.spec.GCMParameterSpec;

public final class MainActivity extends Activity {
    private final ExecutorService worker = Executors.newSingleThreadExecutor();
    private EditText code, text;
    private TextView status;
    private LinearLayout history;
    private JSONObject peer;
    private String deviceId;
    private boolean busy;
    private volatile boolean cancelled;

    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        // 分享 URI 仅在本次 Activity 生命周期内读取；内容不会自动发送。
        LinearLayout root = new LinearLayout(this); root.setOrientation(LinearLayout.VERTICAL); root.setPadding(24,64,24,36);
        ScrollView scroll = new ScrollView(this); scroll.addView(root); setContentView(scroll);
        label(root, "PasteOrbit · 局域网直连\n先在电脑开启设备直连，复制配对码到这里。分块传输，不设总大小上限；每次最多 32 个文件。");
        code = new EditText(this); code.setHint("粘贴电脑配对码（含密钥）"); root.addView(code);
        addButton(root, "配对电脑", () -> { String value = code.getText().toString(); run(() -> {
            JSONObject candidate = Transfer.pairing(value);
            JSONObject hello = Transfer.message("hello").put("deviceId", deviceId).put("name", Build.MODEL);
            Transfer.send(candidate, hello); savePeer(candidate); peer = candidate;
            runOnUiThread(() -> code.setText("")); return "配对成功";
        }); });
        text = new EditText(this); text.setHint("输入文字，或从其他应用分享内容到 PasteOrbit"); text.setMinLines(3); root.addView(text);
        addButton(root, "发送输入文字", () -> { String value = text.getText().toString(); run(() -> send(Transfer.message("text").put("text", value))); });
        addButton(root, "发送本次分享内容", () -> run(() -> send(readShared(getIntent()))));
        addButton(root, "接收电脑发来的内容", () -> run(this::receive));
        addButton(root, "取消当前传输", () -> { cancelled = true; status.setText("正在取消，等待当前块完成…"); });
        addButton(root, "解除本机配对", () -> { if (!busy) { peer = null; getPreferences(0).edit().remove("peer").apply(); status.setText("本机配对已移除；彻底撤销请在电脑重置配对。"); } });
        status = label(root, "尚未配对");
        label(root, "已接收（仅应用打开时接收，不后台读取剪贴板）");
        history = new LinearLayout(this); history.setOrientation(LinearLayout.VERTICAL); root.addView(history);
        deviceId = getPreferences(0).getString("device", null);
        if (deviceId == null) { deviceId = UUID.randomUUID().toString(); getPreferences(0).edit().putString("device", deviceId).apply(); }
        try { peer = loadPeer(); if (peer != null) status.setText("已配对：" + peer.optString("name")); refreshHistory(); }
        catch (Exception e) { status.setText("配置读取失败，请重新配对：" + e.getMessage()); }
    }
    private TextView label(LinearLayout root, String value) { TextView v = new TextView(this); v.setText(value); v.setPadding(0,12,0,12); root.addView(v); return v; }
    private void addButton(LinearLayout root, String title, Runnable click) { Button b = new Button(this); b.setText(title); b.setOnClickListener(v -> click.run()); root.addView(b); }
    private interface Work { String execute() throws Exception; }
    private void run(Work task) {
        if (busy) return; busy = true; cancelled = false; status.setText("处理中…");
        worker.execute(() -> { String result; try { result = task.execute(); } catch (Exception e) { result = "失败：" + e.getMessage(); }
            String finalResult = result; runOnUiThread(() -> { busy = false; status.setText(finalResult); refreshHistory(); }); });
    }
    private String send(JSONObject m) throws Exception {
        try {
            if (peer == null) throw new IOException("请先配对");
            JSONObject reply = Transfer.upload(peer, m, getCacheDir(), () -> cancelled, this::progress);
            if (!"ok".equals(reply.optString("kind"))) throw new IOException("电脑未确认");
            return "已发送到电脑收件箱";
        } finally {
            JSONArray files = m.optJSONArray("files");
            if (files != null) for (int i=0;i<files.length();i++) { String path = files.getJSONObject(i).optString("_path"); if (!path.isEmpty()) new File(path).delete(); }
        }
    }
    private void progress(long bytes) { runOnUiThread(() -> status.setText(String.format(java.util.Locale.ROOT,"已传输 %.1f MiB",bytes/1048576.0))); }
    private JSONObject readShared(Intent intent) throws Exception {
        ArrayList<Uri> uris = new ArrayList<>();
        if (Intent.ACTION_SEND_MULTIPLE.equals(intent.getAction())) {
            ArrayList<Uri> values = intent.getParcelableArrayListExtra(Intent.EXTRA_STREAM); if (values != null) uris.addAll(values);
        } else if (Intent.ACTION_SEND.equals(intent.getAction())) { Uri uri = intent.getParcelableExtra(Intent.EXTRA_STREAM); if (uri != null) uris.add(uri); }
        if (uris.isEmpty()) return Transfer.message("text").put("text", String.valueOf(intent.getCharSequenceExtra(Intent.EXTRA_TEXT) == null ? "" : intent.getCharSequenceExtra(Intent.EXTRA_TEXT)));
        if (uris.size() > 32) throw new IOException("最多 32 个文件");
        JSONArray files = new JSONArray();
        ArrayList<File> temporary = new ArrayList<>();
        try {
        for (Uri uri : uris) {
            if (!"content".equals(uri.getScheme())) throw new IOException("仅接受系统分享的文件 URI");
            String name = "shared-file";
            try (Cursor c = getContentResolver().query(uri, new String[]{OpenableColumns.DISPLAY_NAME}, null, null, null)) { if (c != null && c.moveToFirst()) name = c.getString(0); }
            File file = File.createTempFile("pasteorbit-share-", ".part",getCacheDir()); temporary.add(file);
            try (InputStream in = getContentResolver().openInputStream(uri); OutputStream out = new FileOutputStream(file)) {
                byte[] buffer = new byte[Transfer.CHUNK]; int count;
                while ((count=in.read(buffer)) != -1) { Transfer.checkCancel(() -> cancelled); Transfer.checkSpace(getCacheDir(),count); out.write(buffer,0,count); }
            }
            files.put(new JSONObject().put("name", name).put("_path",file.getAbsolutePath()));
        }
        return Transfer.message(uris.size() == 1 && intent.getType() != null && intent.getType().startsWith("image/") ? "image" : "files").put("files", files);
        } catch (Exception e) { for (File file : temporary) file.delete(); throw e; }
    }
    private String receive() throws Exception {
        if (peer == null) throw new IOException("请先配对");
        JSONObject m = Transfer.send(peer, Transfer.message("poll").put("deviceId", deviceId));
        if ("empty".equals(m.getString("kind"))) return "暂无待接收内容";
        boolean bundle = "bundle".equals(m.getString("kind"));
        if (!bundle) Transfer.validate(m);
        File inbox = new File(getFilesDir(), "inbox"); inbox.mkdirs();
        File folder = new File(inbox, UUID.fromString(m.getString("id")).toString());
        if (!new File(folder, "message.json").exists()) {
            File[] entries = inbox.listFiles(); if (!folder.exists() && entries != null && entries.length >= 30) throw new IOException("手机收件箱已满，请删除旧内容");
            folder.mkdirs();
            try {
            if (bundle) m = Transfer.download(peer,m,folder,() -> cancelled,this::progress);
            JSONArray files = m.getJSONArray("files");
            if (!bundle)
            for (int i=0; i<files.length(); i++) {
                JSONObject f = files.getJSONObject(i); byte[] data = Transfer.decode(f.getString("data"));
                try (FileOutputStream out = new FileOutputStream(new File(folder, (i+1)+"-"+f.getString("name")))) { out.write(data); }
                f.remove("data");
            }
            String preview = "text".equals(m.getString("kind")) ? m.optString("text") : m.getString("kind") + " · " + files.length() + " 个文件";
            try (FileOutputStream previewOut = new FileOutputStream(new File(folder, "preview.txt"))) {
                previewOut.write(preview.substring(0, Math.min(preview.length(), 200)).getBytes(StandardCharsets.UTF_8));
            }
            // 完整落盘后再写完成标记，进程中断时仍可重新领取。
            android.util.AtomicFile metadata = new android.util.AtomicFile(new File(folder, "message.json"));
            FileOutputStream out = metadata.startWrite();
            try { out.write(m.toString().getBytes(StandardCharsets.UTF_8)); metadata.finishWrite(out); }
            catch (Exception e) { metadata.failWrite(out); throw e; }
            } catch (Exception e) {
                File[] partial = folder.listFiles(); if (partial != null) for (File file : partial) file.delete(); folder.delete(); throw e;
            }
        }
        Transfer.send(peer, Transfer.message("ack").put("deviceId", deviceId).put("text", m.getString("id")));
        return "已接收，可复制或分享给其他应用";
    }
    private void refreshHistory() {
        history.removeAllViews(); File[] folders = new File(getFilesDir(), "inbox").listFiles(); if (folders == null) return;
        Arrays.sort(folders, (a,b) -> Long.compare(b.lastModified(), a.lastModified()));
        for (File folder : folders) try {
            if (!new File(folder, "message.json").exists()) { label(history, "未完成收件，请再次接收"); continue; }
            // 列表只加载短预览，按钮不捕获整条大文本，使用时才读取内容。
            String title = new String(Transfer.read(new FileInputStream(new File(folder,"preview.txt")), 2048), StandardCharsets.UTF_8);
            label(history, title);
            addButton(history, "复制到剪贴板", () -> { try { copy(readMessage(folder), folder); status.setText("已复制"); } catch(Exception e) { status.setText(e.getMessage()); } });
            addButton(history, "分享", () -> { try { share(readMessage(folder), folder); } catch(Exception e) { status.setText(e.getMessage()); } });
            addButton(history, "删除此收件", () -> { if (!busy) { File[] files = folder.listFiles(); if(files != null) for(File f:files) f.delete(); folder.delete(); refreshHistory(); } });
        } catch(Exception e) { label(history, "未完成收件，请再次接收"); }
    }
    private JSONObject readMessage(File folder) throws Exception {
        return new JSONObject(new String(Transfer.read(new FileInputStream(new File(folder,"message.json")), 16 * 1024 * 1024), StandardCharsets.UTF_8));
    }
    private ArrayList<Uri> uris(JSONObject m, File folder) throws Exception {
        ArrayList<Uri> result = new ArrayList<>(); JSONArray files = m.getJSONArray("files");
        for(int i=0;i<files.length();i++) result.add(FileProvider.getUriForFile(this,getPackageName()+".files",new File(folder,(i+1)+"-"+files.getJSONObject(i).getString("name")))); return result;
    }
    private void copy(JSONObject m, File folder) throws Exception {
        ClipData clip;
        if ("text".equals(m.getString("kind"))) {
            if (m.optString("text").length() > 128 * 1024) throw new IOException("文本过长，不适合系统剪贴板，请使用分享导出文件");
            clip = ClipData.newPlainText("PasteOrbit",m.optString("text"));
        }
        else { ArrayList<Uri> values=uris(m,folder); clip=ClipData.newUri(getContentResolver(),"PasteOrbit",values.get(0)); for(int i=1;i<values.size();i++) clip.addItem(new ClipData.Item(values.get(i))); }
        ((ClipboardManager)getSystemService(CLIPBOARD_SERVICE)).setPrimaryClip(clip);
    }
    private void share(JSONObject m, File folder) throws Exception {
        Intent intent = new Intent(Intent.ACTION_SEND);
        if("text".equals(m.getString("kind"))) {
            intent.setType("text/plain");
            if (m.optString("text").length() <= 128 * 1024) intent.putExtra(Intent.EXTRA_TEXT,m.optString("text"));
            else {
                File exported = new File(folder, "text.txt");
                try (FileOutputStream out = new FileOutputStream(exported)) { out.write(m.optString("text").getBytes(StandardCharsets.UTF_8)); }
                Uri uri = FileProvider.getUriForFile(this,getPackageName()+".files",exported);
                intent.putExtra(Intent.EXTRA_STREAM,uri); intent.setClipData(ClipData.newUri(getContentResolver(),"PasteOrbit",uri)); intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
            }
        }
        else { ArrayList<Uri> values=uris(m,folder); intent.setAction(Intent.ACTION_SEND_MULTIPLE); intent.setType("image".equals(m.getString("kind"))?"image/*":"*/*"); intent.putParcelableArrayListExtra(Intent.EXTRA_STREAM,values); intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION); ClipData clip=ClipData.newUri(getContentResolver(),"PasteOrbit",values.get(0)); for(int i=1;i<values.size();i++) clip.addItem(new ClipData.Item(values.get(i))); intent.setClipData(clip); }
        startActivity(Intent.createChooser(intent,"分享接收内容"));
    }
    private SecretKey storageKey() throws Exception {
        KeyStore store=KeyStore.getInstance("AndroidKeyStore"); store.load(null);
        if(!store.containsAlias("PasteOrbitPairing")) { KeyGenerator generator=KeyGenerator.getInstance("AES","AndroidKeyStore"); generator.init(new KeyGenParameterSpec.Builder("PasteOrbitPairing",KeyProperties.PURPOSE_ENCRYPT|KeyProperties.PURPOSE_DECRYPT).setBlockModes(KeyProperties.BLOCK_MODE_GCM).setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE).build()); generator.generateKey(); }
        return (SecretKey)store.getKey("PasteOrbitPairing",null);
    }
    private void savePeer(JSONObject value) throws Exception { Cipher c=Cipher.getInstance("AES/GCM/NoPadding"); c.init(Cipher.ENCRYPT_MODE,storageKey()); getPreferences(0).edit().putString("peer",Transfer.encode(c.getIV())+":"+Transfer.encode(c.doFinal(value.toString().getBytes(StandardCharsets.UTF_8)))).commit(); }
    private JSONObject loadPeer() throws Exception { String value=getPreferences(0).getString("peer",null); if(value==null)return null; String[] parts=value.split(":"); Cipher c=Cipher.getInstance("AES/GCM/NoPadding"); c.init(Cipher.DECRYPT_MODE,storageKey(),new GCMParameterSpec(128,Transfer.decode(parts[0]))); return Transfer.pairing(new String(c.doFinal(Transfer.decode(parts[1])),StandardCharsets.UTF_8)); }
    @Override public void onDestroy() { cancelled = true; worker.shutdown(); super.onDestroy(); }
}
