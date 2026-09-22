package com.pasteorbit.mobile;

import android.util.Base64;
import org.json.*;
import java.io.*;
import java.net.*;
import java.nio.charset.StandardCharsets;
import java.security.SecureRandom;
import java.util.*;
import javax.crypto.Cipher;
import javax.crypto.spec.*;

/** 与 Windows 共用 v1 协议，HTTP 内仅承载 AES-GCM 密文。 */
final class Transfer {
    static final int CHUNK = 256 * 1024;
    static final byte[] AAD = "PasteOrbit.Direct/1".getBytes(StandardCharsets.UTF_8);
    static byte[] decode(String s) { return Base64.decode(s, Base64.NO_WRAP); }
    static String encode(byte[] b) { return Base64.encodeToString(b, Base64.NO_WRAP); }

    static JSONObject pairing(String code) throws Exception {
        JSONObject p = new JSONObject(code);
        if (p.getInt("version") != 1 || decode(p.getString("key")).length != 32) throw new IOException("配对码无效");
        URI uri = new URI(p.getString("endpoint"));
        String host = uri.getHost();
        if (!"http".equals(uri.getScheme()) || uri.getRawQuery() != null || uri.getRawFragment() != null
            || uri.getUserInfo() != null || !(uri.getPath().isEmpty() || "/".equals(uri.getPath()))
            || host == null || !host.matches("\\d+\\.\\d+\\.\\d+\\.\\d+")) throw new IOException("需要局域网 IPv4 地址");
        byte[] a = InetAddress.getByName(host).getAddress();
        int x = a[0] & 255, y = a[1] & 255;
        if (!(x == 10 || x == 127 || x == 192 && y == 168 || x == 172 && y >= 16 && y <= 31 || x == 169 && y == 254))
            throw new IOException("仅允许局域网地址");
        return p;
    }
    static JSONObject message(String kind) throws Exception {
        return new JSONObject().put("id", UUID.randomUUID().toString()).put("time", System.currentTimeMillis()/1000)
            .put("kind", kind).put("text", "").put("files", new JSONArray()).put("deviceId", "").put("name", "");
    }
    static byte[] read(InputStream in, int max) throws IOException {
        try (InputStream input = in; ByteArrayOutputStream out = new ByteArrayOutputStream()) {
            byte[] buffer = new byte[65536]; int n;
            while ((n = input.read(buffer)) != -1) { if (out.size() + n > max) throw new IOException("内容超过本次内存读取预算，请使用文件分享"); out.write(buffer, 0, n); }
            return out.toByteArray();
        }
    }
    static void validate(JSONObject m) throws Exception {
        UUID.fromString(m.getString("id"));
        String kind = m.getString("kind"), text = m.optString("text");
        JSONArray files = m.getJSONArray("files");
        if (!Arrays.asList("text", "image", "files").contains(kind) || files.length() > 32
            || kind.equals("text") && (text.isEmpty() || files.length() != 0)
            || !kind.equals("text") && files.length() == 0 || kind.equals("image") && files.length() != 1) throw new IOException("内容无效");
        for (int i = 0; i < files.length(); i++) {
            JSONObject f = files.getJSONObject(i); String name = f.getString("name");
            if (name.isEmpty() || name.length() > 180 || name.equals(".") || name.equals("..")
                || name.endsWith(".") || name.endsWith(" ") || name.chars().anyMatch(c -> c < 32 || "\\/:*?\"<>|".indexOf(c) >= 0)) throw new IOException("文件名无效");
        }
    }

    interface Progress { void report(long bytes); }
    static void checkSpace(File directory, long bytes) throws IOException {
        if (bytes < 0 || directory.getUsableSpace() - 64L * 1024 * 1024 < bytes) throw new IOException("磁盘空间不足，接收内容仍保留在电脑");
    }
    static void checkCancel(java.util.function.BooleanSupplier cancelled) throws IOException {
        if (cancelled.getAsBoolean()) throw new IOException("已取消");
    }
    static JSONObject upload(JSONObject peer, JSONObject message, File cache, java.util.function.BooleanSupplier cancelled, Progress progress) throws Exception {
        validate(message);
        JSONArray sources = message.getJSONArray("files"), parts = new JSONArray();
        File textFile = null;
        try {
            if ("text".equals(message.getString("kind"))) {
                textFile = File.createTempFile("pasteorbit-text-", ".txt", cache);
                // 增量字符编码，避免为大文字再分配完整 UTF-8 数组。
                try (Writer out = new OutputStreamWriter(new FileOutputStream(textFile), StandardCharsets.UTF_8)) {
                    String value = message.getString("text");
                    for (int offset = 0; offset < value.length(); offset += 8192) {
                        checkCancel(cancelled); checkSpace(cache, 32768); out.write(value, offset, Math.min(8192, value.length() - offset));
                    }
                }
                sources = new JSONArray().put(new JSONObject().put("name", "text.txt").put("_path", textFile.getAbsolutePath()));
            }
            for (int i=0; i<sources.length(); i++) {
                JSONObject source = sources.getJSONObject(i);
                parts.put(new JSONObject().put("name",source.getString("name")).put("length",new File(source.getString("_path")).length()));
            }
            JSONObject manifest = new JSONObject().put("kind",message.getString("kind")).put("parts",parts);
            JSONObject begin = send(peer, message("begin").put("text",manifest.toString()));
            String id = begin.getString("deviceId"); UUID.fromString(id);
            long sent = 0;
            try {
                byte[] buffer = new byte[CHUNK];
                for (int i=0; i<sources.length(); i++) {
                    long offset = 0, expected = parts.getJSONObject(i).getLong("length");
                    try (InputStream in = new FileInputStream(sources.getJSONObject(i).getString("_path"))) {
                        int count;
                        while ((count = in.read(buffer)) != -1) {
                            checkCancel(cancelled);
                            if (count > expected - offset) throw new IOException("源文件已变化");
                            send(peer,message("part").put("deviceId",id).put("text",i+":"+offset)
                                .put("files",new JSONArray().put(new JSONObject().put("name","chunk").put("data",encode(Arrays.copyOf(buffer,count))))));
                            offset += count; sent += count; progress.report(sent);
                        }
                    }
                    if (offset != expected) throw new IOException("源文件已变化");
                }
                checkCancel(cancelled);
                return send(peer,message("commit").put("deviceId",id));
            } catch (Exception e) {
                try { send(peer,message("abort").put("deviceId",id)); } catch (Exception ignored) { }
                throw e;
            }
        } finally { if (textFile != null) textFile.delete(); }
    }

    static JSONObject download(JSONObject peer, JSONObject bundle, File folder, java.util.function.BooleanSupplier cancelled, Progress progress) throws Exception {
        JSONObject manifest = new JSONObject(bundle.getString("text"));
        String kind = manifest.getString("kind"); JSONArray parts = manifest.getJSONArray("parts");
        if (parts.length() == 0 || parts.length() > 32 || !Arrays.asList("text","image","files").contains(kind)
            || !"files".equals(kind) && parts.length() != 1) throw new IOException("清单无效");
        JSONArray files = new JSONArray(); long total = 0;
        for (int i=0;i<parts.length();i++) {
            JSONObject part = parts.getJSONObject(i); long length = part.getLong("length");
            if (length < 0) throw new IOException("长度无效"); total = Math.addExact(total,length);
            files.put(new JSONObject().put("name",part.getString("name")));
        }
        validate(message("files").put("files",files)); checkSpace(folder,total);
        long received = 0;
        for (int i=0;i<parts.length();i++) {
            long expected = parts.getJSONObject(i).getLong("length"), offset = 0, chunk = 0;
            try (FileOutputStream out = new FileOutputStream(new File(folder,(i+1)+"-"+parts.getJSONObject(i).getString("name")))) {
                while (offset < expected) {
                    checkCancel(cancelled);
                    JSONObject reply = send(peer,message("read").put("deviceId",bundle.getString("deviceId")).put("text",i+":"+chunk));
                    if (!"chunk".equals(reply.getString("kind")) || reply.getJSONArray("files").length() != 1) throw new IOException("块响应无效");
                    byte[] bytes = decode(reply.getJSONArray("files").getJSONObject(0).getString("data"));
                    if (bytes.length == 0 || bytes.length > CHUNK || bytes.length > expected-offset) throw new IOException("块长度无效");
                    checkSpace(folder,bytes.length); out.write(bytes); offset += bytes.length; received += bytes.length; chunk++; progress.report(received);
                }
                out.getFD().sync();
            }
        }
        JSONObject result = message(kind).put("id",bundle.getString("id")).put("files",files);
        if ("text".equals(kind)) {
            if (parts.getJSONObject(0).getLong("length") <= 128 * 1024) {
                String value = new String(read(new FileInputStream(new File(folder,"1-"+parts.getJSONObject(0).getString("name"))),128*1024),StandardCharsets.UTF_8);
                result.put("text",value).put("files",new JSONArray());
            } else result.put("kind","files");
        }
        return result;
    }
    static JSONObject send(JSONObject p, JSONObject m) throws Exception {
        pairing(p.toString());
        byte[] nonce = new byte[12]; new SecureRandom().nextBytes(nonce);
        Cipher cipher = Cipher.getInstance("AES/GCM/NoPadding");
        SecretKeySpec key = new SecretKeySpec(decode(p.getString("key")), "AES");
        cipher.init(Cipher.ENCRYPT_MODE, key, new GCMParameterSpec(128, nonce)); cipher.updateAAD(AAD);
        byte[] encrypted = cipher.doFinal(m.toString().getBytes(StandardCharsets.UTF_8));
        byte[] body = new JSONObject().put("nonce", encode(nonce))
            .put("data", encode(Arrays.copyOf(encrypted, encrypted.length-16)))
            .put("tag", encode(Arrays.copyOfRange(encrypted, encrypted.length-16, encrypted.length))).toString().getBytes(StandardCharsets.UTF_8);
        if (body.length > 16 * 1024 * 1024) throw new IOException("消息过大");
        String endpoint = p.getString("endpoint").replaceAll("/+$", "") + "/v1/transfer";
        HttpURLConnection c = (HttpURLConnection)new URL(endpoint).openConnection(Proxy.NO_PROXY);
        try {
            c.setInstanceFollowRedirects(false); c.setConnectTimeout(10000); c.setReadTimeout(30000);
            c.setRequestMethod("POST"); c.setDoOutput(true); c.setFixedLengthStreamingMode(body.length);
            c.setRequestProperty("Content-Type", "application/json");
            try (OutputStream out = c.getOutputStream()) { out.write(body); }
            if (c.getResponseCode() != 200) throw new IOException("电脑未接受连接：" + c.getResponseCode());
            JSONObject envelope = new JSONObject(new String(read(c.getInputStream(), 16*1024*1024), StandardCharsets.UTF_8));
            byte[] data = decode(envelope.getString("data")), tag = decode(envelope.getString("tag"));
            if (tag.length != 16) throw new IOException("响应无效");
            byte[] combined = Arrays.copyOf(data, data.length + tag.length); System.arraycopy(tag, 0, combined, data.length, tag.length);
            cipher.init(Cipher.DECRYPT_MODE, key, new GCMParameterSpec(128, decode(envelope.getString("nonce")))); cipher.updateAAD(AAD);
            JSONObject reply = new JSONObject(new String(cipher.doFinal(combined), StandardCharsets.UTF_8));
            if (!m.getString("id").equals(reply.optString("replyTo")) || Math.abs(System.currentTimeMillis()/1000 - reply.getLong("time")) > 300)
                throw new IOException("响应不匹配或已过期");
            if (reply.getString("kind").equals("error")) throw new IOException(reply.optString("text"));
            return reply;
        } finally { c.disconnect(); }
    }
}
