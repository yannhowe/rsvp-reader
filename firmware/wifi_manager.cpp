/*
 * wifi_manager.cpp
 *
 * WiFi AP + web server for uploading/managing EPUB files on the SD card.
 *
 * - Creates "RSVP-Reader" soft-AP (no password, personal device)
 * - mDNS: http://rsvp.local  (iOS/macOS; Android uses 192.168.4.1)
 * - Streaming multipart upload writes directly to SD in ~1.4KB chunks
 * - WiFi off by default; toggled from device menu to save ~150mA
 */

#include "wifi_manager.h"
#include "sd_manager.h"
#include "bookmark_manager.h"

#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <SD_MMC.h>

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

static const char* AP_SSID = "RSVP-Reader";
static const char* MDNS_HOST = "rsvp";

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

static bool s_active = false;
static WebServer* s_server = nullptr;

// Upload state
static File s_upload_file;
static String s_upload_path;
static bool s_upload_ok = false;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Sanitize a filename: strip path separators and reject traversal attempts.
// Returns empty string if the filename is invalid.
static String sanitize_filename(const String& raw) {
    // Take only the part after the last slash or backslash
    int last_sep = raw.lastIndexOf('/');
    int last_bsep = raw.lastIndexOf('\\');
    int start = max(last_sep, last_bsep) + 1;
    String name = raw.substring(start);
    name.trim();

    // Reject empty, dotfiles, or traversal
    if (name.isEmpty() || name.startsWith(".") || name.indexOf("..") >= 0) {
        return String();
    }
    return name;
}

// Append a JSON-escaped string to dst (escapes \ and ").
static void json_escape_append(String& dst, const String& src) {
    for (unsigned int i = 0; i < src.length(); i++) {
        char c = src[i];
        if (c == '"' || c == '\\') dst += '\\';
        dst += c;
    }
}

// ---------------------------------------------------------------------------
// PROGMEM HTML page (~4KB)
// ---------------------------------------------------------------------------

static const char PAGE_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>RSVP Reader</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{background:#111;color:#eee;font-family:-apple-system,system-ui,sans-serif;
  padding:16px;max-width:480px;margin:0 auto}
h1{font-size:1.4em;text-align:center;padding:12px 0;color:#ff4444}
.card{background:#1a1a1a;border-radius:8px;padding:16px;margin:12px 0}
.storage{margin:8px 0}
.bar-bg{background:#333;border-radius:4px;height:8px;overflow:hidden}
.bar-fill{background:#ff4444;height:100%;transition:width .3s}
.storage-text{font-size:.85em;color:#999;margin-top:4px}
.file-list{list-style:none}
.file-item{display:flex;justify-content:space-between;align-items:center;
  padding:10px 0;border-bottom:1px solid #333}
.file-item:last-child{border-bottom:none}
.file-name{flex:1;word-break:break-all;font-size:.95em}
.file-size{color:#999;font-size:.85em;margin:0 12px;white-space:nowrap}
.btn-del{background:#333;color:#ff4444;border:none;border-radius:4px;
  padding:6px 12px;cursor:pointer;font-size:.85em}
.btn-del:hover{background:#ff4444;color:#fff}
.upload-form{text-align:center}
input[type=file]{display:none}
.btn-choose{display:inline-block;background:#333;color:#eee;
  border-radius:6px;padding:12px 24px;cursor:pointer;font-size:1em;margin:8px 0}
.btn-choose:hover{background:#444}
.btn-upload{background:#ff4444;color:#fff;border:none;border-radius:6px;
  padding:12px 24px;font-size:1em;cursor:pointer;margin:8px 0;width:100%}
.btn-upload:hover{background:#cc3333}
.btn-upload:disabled{background:#555;color:#999;cursor:not-allowed}
.selected-file{color:#999;font-size:.85em;margin:4px 0;min-height:1.2em}
.progress{display:none;margin:8px 0}
.progress .bar-bg{height:12px}
.msg{text-align:center;padding:8px;border-radius:4px;margin:8px 0;display:none}
.msg.ok{background:#1a3a1a;color:#4f4;display:block}
.msg.err{background:#3a1a1a;color:#f44;display:block}
.empty{text-align:center;color:#666;padding:20px 0}
</style>
</head>
<body>
<h1>RSVP Reader</h1>

<div class="card">
<h2 style="font-size:1.1em;margin-bottom:8px">Upload EPUB</h2>
<form class="upload-form" id="uploadForm" method="POST" action="/upload" enctype="multipart/form-data">
  <label class="btn-choose" for="fileInput">Choose File</label>
  <input type="file" id="fileInput" name="file" accept=".epub">
  <div class="selected-file" id="selFile"></div>
  <div class="progress" id="prog"><div class="bar-bg"><div class="bar-fill" id="progBar" style="width:0%"></div></div></div>
  <button class="btn-upload" type="submit" id="btnUpload" disabled>Upload</button>
</form>
<div class="msg" id="msg"></div>
</div>

<div class="card">
<h2 style="font-size:1.1em;margin-bottom:8px">Library</h2>
<div class="storage" id="storageArea"></div>
<ul class="file-list" id="fileList"><li class="empty">Loading...</li></ul>
</div>

<script>
const $=s=>document.querySelector(s);
const fileInput=$('#fileInput'),btnUpload=$('#btnUpload'),selFile=$('#selFile');
const msg=$('#msg'),prog=$('#prog'),progBar=$('#progBar');

fileInput.addEventListener('change',()=>{
  const f=fileInput.files[0];
  if(f){selFile.textContent=f.name;btnUpload.disabled=false}
  else{selFile.textContent='';btnUpload.disabled=true}
});

$('#uploadForm').addEventListener('submit',e=>{
  e.preventDefault();
  const f=fileInput.files[0];
  if(!f)return;
  msg.className='msg';msg.style.display='none';
  prog.style.display='block';progBar.style.width='0%';
  btnUpload.disabled=true;

  const xhr=new XMLHttpRequest();
  xhr.open('POST','/upload');
  xhr.upload.onprogress=e=>{if(e.lengthComputable)progBar.style.width=Math.round(e.loaded/e.total*100)+'%'};
  xhr.onload=()=>{
    prog.style.display='none';
    if(xhr.status===200){msg.className='msg ok';msg.textContent='Upload complete!';fileInput.value='';selFile.textContent='';loadFiles()}
    else{msg.className='msg err';msg.textContent='Upload failed: '+xhr.responseText;btnUpload.disabled=false}
  };
  xhr.onerror=()=>{prog.style.display='none';msg.className='msg err';msg.textContent='Network error';btnUpload.disabled=false};

  const fd=new FormData();fd.append('file',f);
  xhr.send(fd);
});

function loadFiles(){
  fetch('/api/files').then(r=>r.json()).then(d=>{
    const list=$('#fileList'),sa=$('#storageArea');
    // Storage bar
    if(d.total_bytes){
      const used=d.total_bytes-d.free_bytes;
      const pct=Math.round(used/d.total_bytes*100);
      sa.innerHTML='<div class="bar-bg"><div class="bar-fill" style="width:'+pct+'%"></div></div>'+
        '<div class="storage-text">'+fmt(used)+' used of '+fmt(d.total_bytes)+' ('+fmt(d.free_bytes)+' free)</div>';
    }
    // File list
    if(!d.files||d.files.length===0){list.innerHTML='<li class="empty">No EPUB files found</li>';return}
    list.innerHTML='';
    d.files.forEach(f=>{
      const li=document.createElement('li');li.className='file-item';
      const nm=f.path.split('/').pop();
      const btn=document.createElement('button');btn.className='btn-del';btn.textContent='Delete';btn.dataset.path=f.path;
      li.innerHTML='<span class="file-name">'+esc(nm)+'</span>'+
        '<span class="file-size">'+esc(f.size_fmt)+'</span>';
      li.appendChild(btn);
      list.appendChild(li);
    });
  }).catch(()=>{$('#fileList').innerHTML='<li class="empty">Error loading files</li>'});
}

function delFile(p){
  if(!confirm('Delete '+p.split('/').pop()+'?'))return;
  fetch('/delete?file='+encodeURIComponent(p)).then(()=>loadFiles());
}

document.addEventListener('click',e=>{
  const btn=e.target.closest('.btn-del');
  if(btn&&btn.dataset.path)delFile(btn.dataset.path);
});

function fmt(b){
  if(b>=1073741824)return(b/1073741824).toFixed(1)+' GB';
  if(b>=1048576)return(b/1048576).toFixed(1)+' MB';
  if(b>=1024)return(b/1024).toFixed(0)+' KB';
  return b+' B';
}

function esc(s){const d=document.createElement('div');d.textContent=s;return d.innerHTML}

loadFiles();
</script>
</body>
</html>
)rawliteral";

// ---------------------------------------------------------------------------
// Handler: GET / — serve the HTML page
// ---------------------------------------------------------------------------

static void handle_root() {
    s_server->send_P(200, "text/html", PAGE_HTML);
}

// ---------------------------------------------------------------------------
// Handler: GET /api/files — JSON file list + storage info
// ---------------------------------------------------------------------------

static void handle_api_files() {
    std::vector<String> epubs = sd_list_epubs("/");

    // Pre-allocate to reduce fragmentation (~300 bytes per entry)
    String json;
    json.reserve(256 + epubs.size() * 300);
    json = "{\"files\":[";
    for (size_t i = 0; i < epubs.size(); i++) {
        if (i > 0) json += ",";
        size_t sz = sd_file_size(epubs[i].c_str());
        json += "{\"path\":\"";
        json_escape_append(json, epubs[i]);
        json += "\",\"size\":";
        json += String(sz);
        json += ",\"size_fmt\":\"";
        json_escape_append(json, sd_format_size(sz));
        json += "\"}";
    }
    // Use snprintf for 64-bit values to avoid truncation
    char storage_buf[80];
    snprintf(storage_buf, sizeof(storage_buf),
             "],\"total_bytes\":%llu,\"free_bytes\":%llu}",
             (unsigned long long)sd_total_bytes(),
             (unsigned long long)sd_free_bytes());
    json += storage_buf;

    s_server->send(200, "application/json", json);
}

// ---------------------------------------------------------------------------
// Handler: POST /upload — streaming multipart upload
// ---------------------------------------------------------------------------

static void handle_upload_done() {
    if (s_upload_ok) {
        s_server->send(200, "text/plain", "OK");
    } else {
        // Clean up partial file on any failure
        if (!s_upload_path.isEmpty() && sd_file_exists(s_upload_path.c_str())) {
            sd_remove_file(s_upload_path.c_str());
            Serial.printf("[WiFi] Cleaned up partial: %s\n", s_upload_path.c_str());
        }
        s_server->send(500, "text/plain", "Upload failed");
    }
}

static void handle_upload_stream() {
    HTTPUpload& upload = s_server->upload();

    switch (upload.status) {
        case UPLOAD_FILE_START: {
            String filename = sanitize_filename(upload.filename);
            if (filename.isEmpty()) {
                Serial.println("[WiFi] Upload rejected: invalid filename");
                s_upload_ok = false;
                return;
            }
            // Only accept .epub files
            String lower = filename;
            lower.toLowerCase();
            if (!lower.endsWith(".epub")) {
                Serial.println("[WiFi] Upload rejected: not an .epub file");
                s_upload_ok = false;
                return;
            }
            // Ensure /books directory exists
            sd_mkdir("/books");
            s_upload_path = "/books/" + filename;
            Serial.printf("[WiFi] Upload start: %s\n", s_upload_path.c_str());

            s_upload_file = SD_MMC.open(s_upload_path.c_str(), FILE_WRITE);
            s_upload_ok = (bool)s_upload_file;
            if (!s_upload_ok) {
                Serial.printf("[WiFi] Failed to open %s for writing\n", s_upload_path.c_str());
            }
            break;
        }

        case UPLOAD_FILE_WRITE: {
            if (s_upload_ok && upload.currentSize > 0) {
                size_t written = s_upload_file.write(upload.buf, upload.currentSize);
                if (written != upload.currentSize) {
                    Serial.println("[WiFi] Write error — disk full?");
                    s_upload_ok = false;
                    s_upload_file.close();
                }
            }
            break;
        }

        case UPLOAD_FILE_END: {
            if (s_upload_ok) {
                s_upload_file.close();
                Serial.printf("[WiFi] Upload complete: %s (%u bytes)\n",
                              s_upload_path.c_str(), upload.totalSize);
            }
            break;
        }

        case UPLOAD_FILE_ABORTED: {
            // Clean up partial file
            if (s_upload_file) {
                s_upload_file.close();
            }
            if (!s_upload_path.isEmpty()) {
                sd_remove_file(s_upload_path.c_str());
                Serial.printf("[WiFi] Upload aborted, removed partial: %s\n",
                              s_upload_path.c_str());
            }
            s_upload_ok = false;
            break;
        }
    }
}

// ---------------------------------------------------------------------------
// Handler: GET /delete?file=/path.epub
// ---------------------------------------------------------------------------

static void handle_delete() {
    if (!s_server->hasArg("file")) {
        s_server->send(400, "text/plain", "Missing file parameter");
        return;
    }

    String path = s_server->arg("file");

    // Safety: only allow deleting .epub files
    String lower = path;
    lower.toLowerCase();
    if (!lower.endsWith(".epub")) {
        s_server->send(403, "text/plain", "Only .epub files can be deleted");
        return;
    }

    if (!sd_file_exists(path.c_str())) {
        s_server->send(404, "text/plain", "File not found");
        return;
    }

    // Delete the bookmark for this book
    bookmark_delete(path.c_str());

    // Delete the file
    if (sd_remove_file(path.c_str())) {
        Serial.printf("[WiFi] Deleted: %s\n", path.c_str());
        s_server->send(200, "text/plain", "OK");
    } else {
        s_server->send(500, "text/plain", "Delete failed");
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void wifi_init() {
    // Pre-init: radio stays off. Nothing to do yet.
    s_active = false;
}

void wifi_start() {
    if (s_active) return;

    Serial.println("[WiFi] Starting AP...");

    WiFi.mode(WIFI_AP);
    WiFi.softAP(AP_SSID);
    delay(100);  // Let DHCP settle

    Serial.printf("[WiFi] AP \"%s\" started — IP: %s\n",
                  AP_SSID, WiFi.softAPIP().toString().c_str());

    // mDNS: http://rsvp.local
    if (MDNS.begin(MDNS_HOST)) {
        MDNS.addService("http", "tcp", 80);
        Serial.printf("[WiFi] mDNS: http://%s.local\n", MDNS_HOST);
    } else {
        Serial.println("[WiFi] mDNS failed — use IP instead");
    }

    // Web server
    s_server = new WebServer(80);
    s_server->on("/", HTTP_GET, handle_root);
    s_server->on("/api/files", HTTP_GET, handle_api_files);
    s_server->on("/upload", HTTP_POST, handle_upload_done, handle_upload_stream);
    s_server->on("/delete", HTTP_GET, handle_delete);
    s_server->begin();

    s_active = true;
    Serial.println("[WiFi] Web server ready");
}

void wifi_stop() {
    if (!s_active) return;

    Serial.println("[WiFi] Stopping...");

    if (s_server) {
        s_server->stop();
        delete s_server;
        s_server = nullptr;
    }

    MDNS.end();
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_OFF);

    s_active = false;
    Serial.println("[WiFi] Stopped");
}

bool wifi_is_active() {
    return s_active;
}

void wifi_tick() {
    if (!s_active || !s_server) return;
    s_server->handleClient();
}

const char* wifi_ssid() {
    return AP_SSID;
}

const char* wifi_ip() {
    static char ip_buf[16];
    if (!s_active) return "";
    strncpy(ip_buf, WiFi.softAPIP().toString().c_str(), sizeof(ip_buf) - 1);
    ip_buf[sizeof(ip_buf) - 1] = '\0';
    return ip_buf;
}
