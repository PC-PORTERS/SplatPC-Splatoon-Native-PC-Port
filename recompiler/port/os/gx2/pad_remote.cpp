// pad_remote.cpp — see pad_remote.h. Windows only (Winsock, WIC for JPEG, BCrypt for SHA-1 and the
// token): no library to add. Other systems get stubs.
#include "pad_remote.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <windows.h>
#include <bcrypt.h>
#include <wincodec.h>
#include "../../runtime/rbrew_diagnostics.h"
#include "../../runtime/screen_mode.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <memory>
#include <mutex>
#include <thread>

void snd_core_set_pad_sink(void (*sink)(const float*, uint32_t)); // snd_core.cpp

namespace pad_remote {
namespace {
constexpr uint32_t kPadW = 854, kPadH = 480;

// The page the phone opens (served at /?t=TOKEN). One touch at a time, like the real GamePad.
const char kPage[] = R"HTML(<!doctype html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,maximum-scale=1,user-scalable=no,viewport-fit=cover">
<meta name="mobile-web-app-capable" content="yes"><meta name="apple-mobile-web-app-capable" content="yes">
<meta name="theme-color" content="#000"><title>Splatoon GamePad</title>
<style>html,body{margin:0;height:100%;background:#000;overflow:hidden;overscroll-behavior:none;touch-action:none;
-webkit-user-select:none;user-select:none;-webkit-touch-callout:none}
#v{position:fixed;inset:0;width:100%;height:100%;object-fit:contain;touch-action:none;-webkit-user-drag:none}
#s{position:fixed;left:10px;top:10px;color:#ddd;font:15px sans-serif;pointer-events:none;text-shadow:0 0 3px #000}</style>
</head><body><img id="v" alt="" draggable="false"><div id="s">Connecting...</div><script>
const t=new URLSearchParams(location.search).get('t')||'';
const v=document.getElementById('v'),s=document.getElementById('s');
let ws=null,shown=null,down=null,lock=null,ac=null,next=0;
// GamePad sound: int16 stereo, 32 kHz, small jitter buffer (starts 0.06 s ahead, drops past 0.2 s).
function sound(){try{if(!ac){const C=window.AudioContext||window.webkitAudioContext;if(C)ac=new C({latencyHint:'interactive'});}
 if(ac&&ac.state!=='running')ac.resume().then(()=>{if(ac.state==='running'&&s.textContent==='Tap the screen for sound')s.textContent='';});}catch(_){}}
function play(buf){
 if(!ac||ac.state!=='running')return;
 const n=(buf.byteLength-1)>>2;if(n<1)return;
 const d=new DataView(buf,1),b=ac.createBuffer(2,n,32000),l=b.getChannelData(0),r=b.getChannelData(1);
 for(let i=0;i<n;i++){l[i]=d.getInt16(i*4,true)/32768;r[i]=d.getInt16(i*4+2,true)/32768;}
 const now=ac.currentTime;
 if(next<now+0.01)next=now+0.06;
 if(next>now+0.2)return;
 const src=ac.createBufferSource();src.buffer=b;src.connect(ac.destination);src.start(next);next+=n/32000;}
function ack(){if(ws&&ws.readyState===1)ws.send('a');}
function connect(){
 ws=new WebSocket('ws://'+location.host+'/ws?t='+encodeURIComponent(t));ws.binaryType='arraybuffer';
 ws.onopen=()=>{s.textContent=ac&&ac.state==='running'?'':'Tap the screen for sound';};
 ws.onmessage=e=>{
  if(typeof e.data==='string'){s.textContent=e.data;return;}
  if(new Uint8Array(e.data,0,1)[0]===1){play(e.data);return;}
  const u=URL.createObjectURL(new Blob([e.data],{type:'image/jpeg'}));
  v.onload=()=>{if(shown)URL.revokeObjectURL(shown);shown=u;ack();};
  v.onerror=()=>{URL.revokeObjectURL(u);ack();};
  v.src=u;};
 ws.onclose=()=>{if(!s.textContent||s.textContent==='')s.textContent='Reconnecting...';down=null;setTimeout(connect,1000);};
}
function pos(e){
 const r=v.getBoundingClientRect(),k=Math.min(r.width/854,r.height/480),w=854*k,h=480*k;
 const x=(e.clientX-r.left-(r.width-w)/2)/w,y=(e.clientY-r.top-(r.height-h)/2)/h;
 return [x,y];}
function send(m){if(ws&&ws.readyState===1)ws.send(m);}
function xy(p){return Math.round(Math.min(Math.max(p[0],0),1)*853)+' '+Math.round(Math.min(Math.max(p[1],0),1)*479);}
async function awake(){try{if('wakeLock' in navigator&&!lock){lock=await navigator.wakeLock.request('screen');lock.onrelease=()=>{lock=null};}}catch(_){}}
function full(){const d=document.documentElement;
 if(!document.fullscreenElement&&d.requestFullscreen)d.requestFullscreen({navigationUI:'hide'}).then(()=>{
  if(screen.orientation&&screen.orientation.lock)screen.orientation.lock('landscape').catch(()=>{});}).catch(()=>{});}
v.addEventListener('pointerdown',e=>{e.preventDefault();sound();full();awake();
 if(down!==null)return;const p=pos(e);if(p[0]<0||p[0]>1||p[1]<0||p[1]>1)return;
 down=e.pointerId;send('d '+xy(p));});
v.addEventListener('pointermove',e=>{if(e.pointerId===down){e.preventDefault();send('m '+xy(pos(e)));}});
function up(e){sound();if(e.pointerId===down){down=null;send('u');}}
v.addEventListener('pointerup',up);v.addEventListener('pointercancel',up);
document.addEventListener('contextmenu',e=>e.preventDefault());
document.addEventListener('visibilitychange',()=>{if(document.hidden){if(down!==null){down=null;send('u');}}else awake();});
connect();
</script></body></html>)HTML";

// --- shared state -----------------------------------------------------------------------------------
std::mutex s_frame_lock;
std::condition_variable s_frame_cv;
std::vector<uint8_t> s_frame; // RGBA8
uint32_t s_frame_w = 0, s_frame_h = 0;
uint64_t s_frame_seq = 0;
std::atomic<bool> s_want{false};   // the sender waits for a picture
std::atomic<bool> s_client{false}; // a phone is connected

std::atomic<bool> s_stop{false};
std::thread& s_server = *new std::thread(); // never destroyed: no std::terminate at exit
SOCKET s_listen = INVALID_SOCKET;
int s_port = 0;
std::string s_token;
std::mutex s_info_lock;
std::string s_error, s_peer;
std::vector<std::string> s_urls;
std::chrono::steady_clock::time_point s_urls_time;

struct Session {
    SOCKET sock = INVALID_SOCKET;
    std::atomic<bool> closed{false};
    std::atomic<bool> acked{true};
    std::mutex send_lock;
    std::thread sender, receiver, audio;
};

// GamePad sound: the audio thread pushes 96-frame blocks (32 kHz stereo); the audio thread of the
// session sends them to the phone every ~20 ms as int16, prefixed with byte 1 (pictures start with 0xFF).
std::mutex s_audio_lock;
std::vector<int16_t> s_audio_ring; // interleaved stereo, capped at 0.2 s
void audio_sink(const float* stereo, uint32_t frames) {
    std::lock_guard lock(s_audio_lock);
    for (uint32_t i = 0; i < frames * 2; ++i)
        s_audio_ring.push_back(int16_t(std::clamp(stereo[i], -1.0f, 1.0f) * 32767.0f));
    constexpr size_t kMax = 32000 * 2 * 2 / 10;
    if (s_audio_ring.size() > kMax) s_audio_ring.erase(s_audio_ring.begin(), s_audio_ring.end() - kMax);
}
Session* s_session = nullptr; // server thread only; never freed at exit (joinable threads)

void set_info(const std::string& error, const std::string& peer) {
    std::lock_guard lock(s_info_lock);
    s_error = error;
    s_peer = peer;
}

// --- helpers ----------------------------------------------------------------------------------------
std::string base64(const uint8_t* data, size_t size) {
    static const char kChars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (size_t i = 0; i < size; i += 3) {
        const uint32_t n = (uint32_t(data[i]) << 16) | (i + 1 < size ? uint32_t(data[i + 1]) << 8 : 0) |
                           (i + 2 < size ? uint32_t(data[i + 2]) : 0);
        out += kChars[(n >> 18) & 63];
        out += kChars[(n >> 12) & 63];
        out += i + 1 < size ? kChars[(n >> 6) & 63] : '=';
        out += i + 2 < size ? kChars[n & 63] : '=';
    }
    return out;
}
bool sha1(const std::string& text, uint8_t out[20]) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA1_ALGORITHM, nullptr, 0) != 0) return false;
    const bool ok = BCryptHash(alg, nullptr, 0, (PUCHAR)text.data(), ULONG(text.size()), out, 20) == 0;
    BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}
std::string random_token() {
    uint8_t bytes[8] = {};
    BCryptGenRandom(nullptr, bytes, sizeof(bytes), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    static const char kHex[] = "0123456789abcdef";
    std::string out;
    for (uint8_t b : bytes) {
        out += kHex[b >> 4];
        out += kHex[b & 15];
    }
    return out;
}
// Only this local network may connect (and the PC itself, for a test in its own browser).
bool private_peer(const sockaddr_in& a) {
    const uint32_t ip = ntohl(a.sin_addr.s_addr);
    return (ip >> 24) == 127 || (ip >> 24) == 10 || (ip >> 20) == 0xAC1 || (ip >> 16) == 0xC0A8 ||
           (ip >> 16) == 0xA9FE;
}
std::string ip_text(const in_addr& a) {
    char text[INET_ADDRSTRLEN] = {};
    inet_ntop(AF_INET, &a, text, sizeof(text));
    return text;
}
bool send_all(SOCKET sock, const void* data, size_t size) {
    const char* p = static_cast<const char*>(data);
    while (size) {
        const int n = ::send(sock, p, int(std::min<size_t>(size, 1 << 20)), 0);
        if (n <= 0) return false;
        p += n;
        size -= size_t(n);
    }
    return true;
}
bool recv_all(SOCKET sock, void* data, size_t size) {
    char* p = static_cast<char*>(data);
    while (size) {
        const int n = ::recv(sock, p, int(size), 0);
        if (n <= 0) return false;
        p += n;
        size -= size_t(n);
    }
    return true;
}
bool ws_send(Session& s, uint8_t opcode, const void* data, size_t size) {
    uint8_t head[10];
    size_t head_size = 2;
    head[0] = uint8_t(0x80 | opcode);
    if (size < 126) {
        head[1] = uint8_t(size);
    } else if (size < 65536) {
        head[1] = 126;
        head[2] = uint8_t(size >> 8);
        head[3] = uint8_t(size);
        head_size = 4;
    } else {
        head[1] = 127;
        for (int i = 0; i < 8; ++i) head[2 + i] = uint8_t(uint64_t(size) >> (56 - 8 * i));
        head_size = 10;
    }
    std::lock_guard lock(s.send_lock);
    return send_all(s.sock, head, head_size) && send_all(s.sock, data, size);
}

// --- JPEG (WIC) -------------------------------------------------------------------------------------
struct Jpeg {
    IWICImagingFactory* factory = nullptr;
    std::vector<uint8_t> bgr;
    float quality = 0.7f;
    bool init() {
        if (const char* q = std::getenv("RBREW_PHONE_JPEG_QUALITY")) quality = std::clamp(float(std::atof(q)), 0.1f, 1.0f);
        return SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                          IID_PPV_ARGS(&factory)));
    }
    ~Jpeg() {
        if (factory) factory->Release();
    }
    bool encode(const uint8_t* rgba, uint32_t w, uint32_t h, std::vector<uint8_t>& out) {
        bgr.resize(size_t(w) * h * 3);
        for (size_t i = 0, j = 0, n = size_t(w) * h; i < n; ++i, j += 3) {
            bgr[j] = rgba[i * 4 + 2];
            bgr[j + 1] = rgba[i * 4 + 1];
            bgr[j + 2] = rgba[i * 4];
        }
        IStream* stream = nullptr;
        IWICBitmapEncoder* encoder = nullptr;
        IWICBitmapFrameEncode* frame = nullptr;
        IPropertyBag2* props = nullptr;
        bool ok = SUCCEEDED(CreateStreamOnHGlobal(nullptr, TRUE, &stream)) &&
                  SUCCEEDED(factory->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, &encoder)) &&
                  SUCCEEDED(encoder->Initialize(stream, WICBitmapEncoderNoCache)) &&
                  SUCCEEDED(encoder->CreateNewFrame(&frame, &props));
        if (ok) {
            PROPBAG2 option{};
            option.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
            VARIANT value;
            VariantInit(&value);
            value.vt = VT_R4;
            value.fltVal = quality;
            props->Write(1, &option, &value);
            WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
            ok = SUCCEEDED(frame->Initialize(props)) && SUCCEEDED(frame->SetSize(w, h)) &&
                 SUCCEEDED(frame->SetPixelFormat(&format)) && IsEqualGUID(format, GUID_WICPixelFormat24bppBGR) &&
                 SUCCEEDED(frame->WritePixels(h, w * 3, UINT(bgr.size()), bgr.data())) &&
                 SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit());
        }
        if (ok) {
            HGLOBAL memory = nullptr;
            STATSTG stat{};
            ok = SUCCEEDED(GetHGlobalFromStream(stream, &memory)) && SUCCEEDED(stream->Stat(&stat, STATFLAG_NONAME));
            if (ok) {
                const void* bytes = GlobalLock(memory);
                out.assign(static_cast<const uint8_t*>(bytes), static_cast<const uint8_t*>(bytes) + stat.cbSize.LowPart);
                GlobalUnlock(memory);
            }
        }
        if (props) props->Release();
        if (frame) frame->Release();
        if (encoder) encoder->Release();
        if (stream) stream->Release();
        return ok;
    }
};

// --- one phone ----------------------------------------------------------------------------------------
void set_phone_touch(bool down, int x, int y) {
    x = std::clamp(x, 0, int(kPadW) - 1);
    y = std::clamp(y, 0, int(kPadH) - 1);
    g_phone_touch.store(down ? (0x80000000u | (uint32_t(y) << 16) | uint32_t(x)) : 0, std::memory_order_relaxed);
}

void sender_loop(Session* s) {
    const bool com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
    {
        Jpeg jpeg;
        if (!jpeg.init()) {
            RBREW_LOG_ERROR("phone", "JPEG encoder (WIC) unavailable\n");
            s->closed = true;
        }
        int fps = 30;
        if (const char* f = std::getenv("RBREW_PHONE_FPS")) fps = std::clamp(std::atoi(f), 5, 60);
        const auto interval = std::chrono::microseconds(1000000 / fps);
        auto last = std::chrono::steady_clock::now() - interval;
        uint64_t seen = 0;
        std::vector<uint8_t> rgba, out;
        uint32_t w = 0, h = 0;
        unsigned frames = 0;
        double encode_ms = 0;
        size_t bytes = 0;
        auto report = std::chrono::steady_clock::now();
        while (!s->closed) {
            // The phone shows the previous picture first (it answers "a"); after 1 s, send anyway.
            for (int i = 0; i < 1000 && !s->acked && !s->closed; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
            const auto wake = last + interval;
            if (std::chrono::steady_clock::now() < wake) std::this_thread::sleep_until(wake);
            {
                std::unique_lock lock(s_frame_lock);
                s_want = true;
                s_frame_cv.wait_for(lock, std::chrono::milliseconds(500), [&] { return s_frame_seq != seen || s->closed; });
                s_want = false;
                if (s_frame_seq == seen || s->closed) continue;
                seen = s_frame_seq;
                rgba.swap(s_frame);
                w = s_frame_w;
                h = s_frame_h;
            }
            last = std::chrono::steady_clock::now();
            if (rgba.size() < size_t(w) * h * 4 || !jpeg.encode(rgba.data(), w, h, out)) continue;
            encode_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - last).count();
            s->acked = false;
            if (!ws_send(*s, 2, out.data(), out.size())) s->closed = true;
            ++frames;
            bytes += out.size();
            if (std::chrono::steady_clock::now() - report > std::chrono::seconds(10)) {
                RBREW_LOG_INFO("phone", "%.1f pictures/s, %zu KB each, encode %.1f ms\n", frames / 10.0,
                               frames ? bytes / frames / 1024 : 0, frames ? encode_ms / frames : 0.0);
                frames = 0;
                bytes = 0;
                encode_ms = 0;
                report = std::chrono::steady_clock::now();
            }
        }
    }
    if (com) CoUninitialize();
    shutdown(s->sock, SD_BOTH); // the receiver stops too
}

void audio_loop(Session* s) {
    std::vector<uint8_t> out;
    std::vector<int16_t> chunk;
    while (!s->closed) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        {
            std::lock_guard lock(s_audio_lock);
            chunk.swap(s_audio_ring);
        }
        if (chunk.empty()) continue;
        out.resize(1 + chunk.size() * 2);
        out[0] = 1;
        for (size_t i = 0; i < chunk.size(); ++i) {
            out[1 + i * 2] = uint8_t(uint16_t(chunk[i]) & 255);
            out[2 + i * 2] = uint8_t(uint16_t(chunk[i]) >> 8);
        }
        chunk.clear();
        if (!ws_send(*s, 2, out.data(), out.size())) {
            s->closed = true;
            shutdown(s->sock, SD_BOTH);
        }
    }
}

void receiver_loop(Session* s) {
    std::vector<uint8_t> payload;
    while (!s->closed) {
        uint8_t head[2];
        if (!recv_all(s->sock, head, 2)) break;
        const uint8_t opcode = head[0] & 15;
        uint64_t size = head[1] & 127;
        if (!(head[1] & 0x80)) break; // a client always masks
        if (size == 126) {
            uint8_t ext[2];
            if (!recv_all(s->sock, ext, 2)) break;
            size = (uint64_t(ext[0]) << 8) | ext[1];
        } else if (size == 127) {
            break; // never this big from our page
        }
        if (size > 1024) break;
        uint8_t mask[4];
        payload.resize(size_t(size));
        if (!recv_all(s->sock, mask, 4) || (size && !recv_all(s->sock, payload.data(), payload.size()))) break;
        for (size_t i = 0; i < payload.size(); ++i) payload[i] ^= mask[i & 3];
        if (opcode == 8) break;                                                  // close
        if (opcode == 9) { ws_send(*s, 10, payload.data(), payload.size()); continue; } // ping
        if (opcode != 1) continue;
        const std::string text(payload.begin(), payload.end());
        int x = 0, y = 0;
        if (text == "a") s->acked = true;
        else if (text == "u") set_phone_touch(false, 0, 0);
        else if ((text[0] == 'd' || text[0] == 'm') && std::sscanf(text.c_str() + 1, "%d %d", &x, &y) == 2)
            set_phone_touch(true, x, y);
    }
    set_phone_touch(false, 0, 0);
    s->closed = true;
    s_frame_cv.notify_all();
}

void end_session() {
    if (!s_session) return;
    s_session->closed = true;
    s_frame_cv.notify_all();
    shutdown(s_session->sock, SD_BOTH);
    if (s_session->sender.joinable()) s_session->sender.join();
    if (s_session->receiver.joinable()) s_session->receiver.join();
    if (s_session->audio.joinable()) s_session->audio.join();
    snd_core_set_pad_sink(nullptr);
    closesocket(s_session->sock);
    delete s_session;
    s_session = nullptr;
    s_client = false;
    set_phone_touch(false, 0, 0);
}

// --- HTTP ---------------------------------------------------------------------------------------------
void reply(SOCKET sock, const char* status, const char* type, const std::string& body) {
    std::string text = std::string("HTTP/1.1 ") + status + "\r\nContent-Type: " + type +
                       "\r\nCache-Control: no-store\r\nConnection: close\r\nContent-Length: " +
                       std::to_string(body.size()) + "\r\n\r\n" + body;
    send_all(sock, text.data(), text.size());
}
std::string header(const std::string& request, const char* name) {
    std::string lower = request;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    const std::string key = std::string("\r\n") + name + ":";
    const size_t at = lower.find(key);
    if (at == std::string::npos) return "";
    size_t begin = at + key.size();
    const size_t end = request.find("\r\n", begin);
    while (begin < end && request[begin] == ' ') ++begin;
    return request.substr(begin, end - begin);
}
std::string query_token(const std::string& target) {
    const size_t at = target.find("t=");
    if (at == std::string::npos || (at && target[at - 1] != '?' && target[at - 1] != '&')) return "";
    return target.substr(at + 2, target.find('&', at) - (at + 2));
}

// Handles one connection: the page, or the WebSocket that becomes the session.
void handle(SOCKET sock, const sockaddr_in& peer) {
    DWORD timeout = 3000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    std::string request;
    char buffer[2048];
    while (request.find("\r\n\r\n") == std::string::npos && request.size() < 8192) {
        const int n = recv(sock, buffer, sizeof(buffer), 0);
        if (n <= 0) break;
        request.append(buffer, size_t(n));
    }
    const size_t space = request.find(' '), space2 = request.find(' ', space + 1);
    if (request.compare(0, 4, "GET ") || space2 == std::string::npos) {
        closesocket(sock);
        return;
    }
    const std::string target = request.substr(space + 1, space2 - space - 1);
    const std::string path = target.substr(0, target.find('?'));
    const bool token_ok = query_token(target) == s_token;
    if (path == "/" || path == "/index.html") {
        if (token_ok) reply(sock, "200 OK", "text/html; charset=utf-8", kPage);
        else reply(sock, "403 Forbidden", "text/plain; charset=utf-8",
                   "Wrong link: open the address shown in the game (ESC menu, GamePad on phone).");
        closesocket(sock);
        return;
    }
    const std::string key = header(request, "sec-websocket-key");
    if (path != "/ws" || key.empty() || !token_ok) {
        reply(sock, token_ok ? "404 Not Found" : "403 Forbidden", "text/plain", "");
        closesocket(sock);
        return;
    }
    uint8_t digest[20];
    if (!sha1(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11", digest)) {
        closesocket(sock);
        return;
    }
    const std::string accept = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                               "Sec-WebSocket-Accept: " + base64(digest, 20) + "\r\n\r\n";
    if (!send_all(sock, accept.data(), accept.size())) {
        closesocket(sock);
        return;
    }
    // The newest phone takes over (one GamePad).
    if (s_session) {
        static const char kTaken[] = "Another device took over the GamePad.";
        ws_send(*s_session, 1, kTaken, sizeof(kTaken) - 1);
        end_session();
    }
    timeout = 0; // the session waits as long as the phone stays
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    const BOOL nodelay = TRUE;
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&nodelay), sizeof(nodelay));
    s_session = new Session();
    s_session->sock = sock;
    s_session->sender = std::thread(sender_loop, s_session);
    s_session->receiver = std::thread(receiver_loop, s_session);
    {
        std::lock_guard lock(s_audio_lock);
        s_audio_ring.clear();
    }
    s_session->audio = std::thread(audio_loop, s_session);
    snd_core_set_pad_sink(audio_sink);
    s_client = true;
    set_info("", ip_text(peer.sin_addr));
    RBREW_LOG_INFO("phone", "GamePad phone connected: %s\n", ip_text(peer.sin_addr).c_str());
}

void server_loop() {
    while (!s_stop) {
        if (s_session && s_session->closed) {
            end_session();
            set_info("", "");
            RBREW_LOG_INFO("phone", "GamePad phone disconnected\n");
        }
        fd_set read;
        FD_ZERO(&read);
        FD_SET(s_listen, &read);
        timeval wait{0, 200000};
        if (select(0, &read, nullptr, nullptr, &wait) <= 0) continue;
        sockaddr_in peer{};
        int size = sizeof(peer);
        const SOCKET sock = accept(s_listen, reinterpret_cast<sockaddr*>(&peer), &size);
        if (sock == INVALID_SOCKET) continue;
        if (!private_peer(peer)) {
            closesocket(sock);
            continue;
        }
        handle(sock, peer);
    }
    end_session();
}

bool virtual_adapter(const IP_ADAPTER_ADDRESSES* a) {
    std::wstring name = std::wstring(a->FriendlyName ? a->FriendlyName : L"") + L" " +
                        (a->Description ? a->Description : L"");
    for (auto& c : name) c = wchar_t(std::towlower(c));
    static const wchar_t* const kSkip[] = {L"virtual", L"hyper-v", L"vethernet", L"vpn",       L"wsl",
                                           L"vmware",  L"vbox",    L"tap",       L"tailscale", L"zerotier",
                                           L"nord",    L"wireguard", L"bluetooth", L"loopback", L"radmin",
                                           L"hamachi"};
    for (const wchar_t* skip : kSkip)
        if (name.find(skip) != std::wstring::npos) return true;
    return false;
}
std::vector<std::string> find_urls() {
    std::vector<std::pair<int, std::string>> found;
    ULONG size = 16384;
    std::vector<uint8_t> buffer(size);
    auto* list = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
    ULONG result = GetAdaptersAddresses(AF_INET, GAA_FLAG_INCLUDE_GATEWAYS | GAA_FLAG_SKIP_DNS_SERVER, nullptr, list, &size);
    if (result == ERROR_BUFFER_OVERFLOW) {
        buffer.resize(size);
        list = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
        result = GetAdaptersAddresses(AF_INET, GAA_FLAG_INCLUDE_GATEWAYS | GAA_FLAG_SKIP_DNS_SERVER, nullptr, list, &size);
    }
    if (result == NO_ERROR)
        for (auto* a = list; a; a = a->Next) {
            if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
            for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
                const auto* in = reinterpret_cast<const sockaddr_in*>(u->Address.lpSockaddr);
                if (in->sin_family != AF_INET || !private_peer(*in) || (ntohl(in->sin_addr.s_addr) >> 24) == 127) continue;
                int score = 0;
                if (!virtual_adapter(a)) score += 4;
                if (a->FirstGatewayAddress) score += 2;
                if (a->IfType == IF_TYPE_IEEE80211) score += 1;
                if ((ntohl(in->sin_addr.s_addr) >> 16) == 0xA9FE) score -= 6; // 169.254: no network
                found.push_back({score, "http://" + ip_text(in->sin_addr) + ":" + std::to_string(s_port) + "/?t=" + s_token});
            }
        }
    std::stable_sort(found.begin(), found.end(), [](auto& a, auto& b) { return a.first > b.first; });
    std::vector<std::string> out;
    for (auto& f : found) out.push_back(f.second);
    if (out.empty()) out.push_back("http://127.0.0.1:" + std::to_string(s_port) + "/?t=" + s_token);
    return out;
}
} // namespace

void start(int port) {
    if (s_server.joinable()) stop();
    WSADATA data;
    WSAStartup(MAKEWORD(2, 2), &data);
    s_port = port;
    s_token = random_token();
    s_stop = false;
    s_listen = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(uint16_t(port));
    if (s_listen == INVALID_SOCKET || bind(s_listen, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
        listen(s_listen, 4) != 0) {
        set_info("Port " + std::to_string(port) + " is busy: change phone_port in settings.ini.", "");
        RBREW_LOG_ERROR("phone", "GamePad phone server: port %d unavailable (%d)\n", port, WSAGetLastError());
        if (s_listen != INVALID_SOCKET) closesocket(s_listen);
        s_listen = INVALID_SOCKET;
        WSACleanup();
        return;
    }
    set_info("", "");
    {
        std::lock_guard lock(s_info_lock);
        s_urls = find_urls();
        s_urls_time = std::chrono::steady_clock::now();
    }
    s_server = std::thread(server_loop);
    RBREW_LOG_INFO("phone", "GamePad phone server on port %d\n", port);
}

void stop() {
    if (!s_server.joinable()) {
        set_info("", "");
        return;
    }
    s_stop = true;
    s_server.join(); // ends the session too
    closesocket(s_listen);
    s_listen = INVALID_SOCKET;
    set_info("", "");
    WSACleanup();
}

bool running() { return s_server.joinable(); }
int port() { return s_port; }
bool wants_frames() { return s_client.load(std::memory_order_relaxed); }

void sink(const uint8_t* rgba, uint32_t width, uint32_t height) {
    if (!s_want.load(std::memory_order_relaxed)) return;
    std::unique_lock lock(s_frame_lock, std::try_to_lock);
    if (!lock.owns_lock() || !s_want) return; // never wait on the render thread
    s_frame.assign(rgba, rgba + size_t(width) * height * 4);
    s_frame_w = width;
    s_frame_h = height;
    ++s_frame_seq;
    s_want = false;
    lock.unlock();
    s_frame_cv.notify_one();
}

std::vector<std::string> urls() {
    std::lock_guard lock(s_info_lock);
    if (!running()) return {};
    if (std::chrono::steady_clock::now() - s_urls_time > std::chrono::seconds(5)) { // Wi-Fi may change
        s_urls = find_urls();
        s_urls_time = std::chrono::steady_clock::now();
    }
    return s_urls;
}

std::string status() {
    std::lock_guard lock(s_info_lock);
    if (!s_error.empty()) return s_error;
    if (!running()) return "";
    if (!s_peer.empty()) return "Phone connected (" + s_peer + ").";
    return "Open the address on a phone on the same Wi-Fi. Nothing loads? Allow Splatoon in the Windows "
           "firewall for private networks.";
}
} // namespace pad_remote

#else
#include "../../runtime/screen_mode.h"
namespace pad_remote {
void start(int) {}
void stop() {}
bool running() { return false; }
int port() { return 0; }
bool wants_frames() { return false; }
void sink(const uint8_t*, uint32_t, uint32_t) {}
std::vector<std::string> urls() { return {}; }
std::string status() { return "Windows only for now."; }
} // namespace pad_remote
#endif
