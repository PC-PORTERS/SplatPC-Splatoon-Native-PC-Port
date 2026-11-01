#include "../../runtime/rbrew_diagnostics.h"
// nsysnet / nn_ac / nlibcurl
//
// Maps Wii U nsysnet functions to POSIX/BSD sockets so game network code
// runs against real TCP/IP.
//
// Wii U sockaddr_in layout (BSD, big-endian in guest memory):
//   +0: uint16 sin_family (big-endian AF_INET = 2)
//   +2: uint16 sin_port   (network byte order)
//   +4: uint32 sin_addr   (network byte order)
//   +8: uint8  sin_zero[8]
//
// gethostbyname / getaddrinfo results are written into a static scratch
// area at NET_SCRATCH_BASE (0x00100000) which lies below all game code
// (RPX text starts at 0x02000000).
//
// nn_ac (network availability) stubs:
//   IsApplicationConnected → *result = true; return 0
//   Initialize/Connect/Close/GetStatus → return 0 (success)
//
// nlibcurl: curl_global_init_mem → 0 (CURLE_OK); cleanup → no-op

#include "../os_common.h"

#if defined(_WIN32)
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <mstcpip.h>
#  ifndef SIO_UDP_CONNRESET
#    define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#  endif
#  pragma comment(lib, "ws2_32")
typedef int              socklen_t;
typedef unsigned short   sa_family_t;
typedef int              ssize_t;
#  define CLOSE_SOCKET(fd) closesocket(fd)
#  define SOCK_ERRNO       WSAGetLastError()
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <poll.h>
#define CLOSE_SOCKET(fd) close(fd)
#define SOCK_ERRNO errno
#define INVALID_SOCKET (-1)
#endif

#include <string.h>
#include <string>
#include <algorithm>
#include <array>
#include <atomic>
#include <utility>
#include <chrono>
#include <cstdlib>
#include <mutex>
#include <stdio.h>
#include "net_policy.h"
#include "socket_abi.h"
#include <thread>
#include <climits>
#include <charconv>
#include "../../runtime/online_identity.h"

// ---------------------------------------------------------------------------
// Confinement (net_policy.h), guest descriptors, and RBREW_NET_TRACE output.
// ---------------------------------------------------------------------------
#if defined(_WIN32)
using HostSocket = SOCKET;
static void set_socket_error(int code) { WSASetLastError(code); }
static constexpr int kErrBlocked = WSAEACCES, kErrBadFd = WSAENOTSOCK;
#else
using HostSocket = int;
static void set_socket_error(int code) { errno = code; }
static constexpr int kErrBlocked = EACCES, kErrBadFd = ENOTSOCK;
#endif
static rbrew_net::FdTable<HostSocket, HostSocket(INVALID_SOCKET)> s_fds;
// Online bench mode (step 5): when online_server is a non-loopback address, the policy
// stops rewriting binds and accepts only loopback + the addresses announced by the operator
// (NEX host, nncs1/nncs2 from RBREW_DNS_OVERRIDE, peers from RBREW_PEERS) + peers explicitly
// allow-listed through rbrew_net::allow_peer() from matchmaking station URLs.
static const rbrew_net::Policy& net_policy() {
    static const rbrew_net::Policy policy = [] {
        const auto config = rbrew_online::config();
        uint32_t server = 0;
        const bool online = config.online &&
            rbrew_net::Policy::parse_ipv4(config.host, server) &&
            !rbrew_net::Policy::is_loopback(server);
        std::vector<uint32_t> announced;
        if (online) {
            announced.push_back(server);
            rbrew_net::Policy overrides(std::getenv("RBREW_DNS_OVERRIDE"));
            for (uint32_t address : overrides.addresses())
                if (!rbrew_net::Policy::is_loopback(address)) announced.push_back(address);
            if (const char* list = std::getenv("RBREW_PEERS")) {
                std::string text(list), item;
                for (size_t start = 0; start <= text.size();) {
                    const size_t end = text.find(',', start);
                    item = text.substr(start, end == std::string::npos ? std::string::npos : end - start);
                    uint32_t address = 0;
                    if (rbrew_net::Policy::parse_ipv4(item, address)) announced.push_back(address);
                    if (end == std::string::npos) break;
                    start = end + 1;
                }
            }
        }
        return rbrew_net::Policy(std::getenv("RBREW_DNS_OVERRIDE"), online,
                                 rbrew_net::steady_seconds, announced);
    }();
    return policy;
}

// Peers announced by matchmaking (station URL addresses) are allow-listed explicitly by a
// host-side hook. There is deliberately NO learning from inbound datagrams: whichever host
// sends one UDP packet could otherwise add itself (bounded to 300 s and 256 entries, but
// unchecked), and receive whatever the game then sends to that address. Without a hook, peers
// stay blocked (safe) until RBREW_PEERS or a matchmaking watcher calls this function.
namespace rbrew_net {
void allow_peer(uint32_t address) { net_policy().add_peer(address); }
}
// Host socket behind a guest descriptor; INVALID_SOCKET (and ENOTSOCK) if none.
static HostSocket host_socket(uint32_t guest_fd) {
    const HostSocket s = s_fds.get(int(guest_fd));
    if (s == HostSocket(INVALID_SOCKET)) set_socket_error(kErrBadFd);
    return s;
}
static thread_local uint32_t s_guest_socket_error=0;
static std::mutex s_socket_io_mutex;
struct SocketMode {bool nonblocking=false;uint64_t generation=0;};
static std::array<SocketMode,32> s_socket_modes{};
#if defined(_WIN32)
static constexpr int kErrFault=WSAEFAULT,kErrInvalid=WSAEINVAL,kErrFamily=WSAEAFNOSUPPORT,kErrFull=WSAEMFILE;
#else
static constexpr int kErrFault=EFAULT,kErrInvalid=EINVAL,kErrFamily=EAFNOSUPPORT,kErrFull=EMFILE;
#endif
static bool guest_buffer(CPUState* cpu,uint32_t p,uint32_t n){return (!n||p)&&uint64_t(p)+n<=cpu->guest_memory_size&&rbrew_memory_accessible(cpu->mem,p,n);}
// B-01: copy a NUL-terminated guest string without ever reading outside guest memory.
// False when the buffer or its terminator is outside; a too-long string is truncated.
static bool guest_string(CPUState* cpu,uint32_t p,char* out,size_t cap){
 if(!p||!cap)return false;
 for(size_t i=0;i+1<cap;++i){
  if(!guest_buffer(cpu,p+uint32_t(i),1u))return false;
  const char c=char(MEM[p+uint32_t(i)]);out[i]=c;
  if(!c)return true;
 }
 out[cap-1]=0;return true;
}
static void net_fail(CPUState* cpu,int e){set_socket_error(e);s_guest_socket_error=rbrew_net::socket_error(e);cpu->r[3]=~0u;}
static void net_result(CPUState* cpu,int rc){const int e=rc<0?SOCK_ERRNO:0;s_guest_socket_error=rbrew_net::socket_error(e);cpu->r[3]=uint32_t(rc);}
// Native sockets never block under the lock; generation checks protect close/reuse.
template<class F> static int socket_io(uint32_t fd,uint32_t flags,F&& call){
 int hf=0;if(!rbrew_net::message_flags(flags,hf)){set_socket_error(kErrInvalid);s_guest_socket_error=rbrew_net::socket_error(kErrInvalid);return -1;}
 uint64_t generation=0;bool first=true;
 for(;;){
  {std::lock_guard lock(s_socket_io_mutex);const auto h=s_fds.get(int(fd));
   if(h==HostSocket(INVALID_SOCKET)||(!first&&generation!=s_socket_modes[fd].generation)){set_socket_error(kErrBadFd);s_guest_socket_error=rbrew_net::socket_error(kErrBadFd);return -1;}
   generation=s_socket_modes[fd].generation;first=false;
   const int rc=call(h,hf),e=rc<0?SOCK_ERRNO:0;s_guest_socket_error=rbrew_net::socket_error(e);
   if(rc>=0||s_guest_socket_error!=6||s_socket_modes[fd].nonblocking||(flags&rbrew_net::kMsgDontWait))return rc;
  }std::this_thread::sleep_for(std::chrono::milliseconds(1));
 }
}

// RBREW_NET_TRACE=<file>: calls, packets (direction, peer, size, first bytes)
// and confinement decisions. Payload bytes after the PRUDP header are RC4
// encrypted; only 32 bytes are written.
static FILE* net_trace_file() {
    static FILE* const file = [] { const char* p = std::getenv("RBREW_NET_TRACE"); return p && *p ? std::fopen(p, "w") : nullptr; }();
    return file;
}
static std::mutex s_trace_lock;
static double net_trace_time() {
    static const auto start = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}
static void net_note(const char* what, const char* detail) {
    FILE* f = net_trace_file();
    if (!f) return;
    std::lock_guard guard(s_trace_lock);
    std::fprintf(f, "%9.3f %-24s %s\n", net_trace_time(), what, detail);
    std::fflush(f);
}
void rbrew_net_note(const char* what, const char* detail) { net_note(what, detail); }   // online_identity.h

// RBREW_NET_PROBE=addr,addr,... (hex, with RBREW_NET_TRACE): logs every
// indirect entry to these guest functions (step callbacks, virtual calls)
// with r3..r5, then the returned r3. Wraps the dispatch-table entry; direct
// calls from generated code are not seen. Diagnostic only, read-only.
namespace {
struct NetProbe { uint32_t address = 0; HostFunc original = nullptr; };
constexpr unsigned kNetProbes = 16;
NetProbe s_net_probes[kNetProbes];
template <unsigned I> void net_probe_entry(CPUState* cpu) {
    const NetProbe& p = s_net_probes[I];
    char text[96];
    std::snprintf(text, sizeof text, "%08X r3=%08X r4=%08X r5=%08X lr=%08X", p.address, cpu->r[3], cpu->r[4], cpu->r[5], cpu->lr);
    net_note("probe enter", text);
    p.original(cpu);
    std::snprintf(text, sizeof text, "%08X -> r3=%08X", p.address, cpu->r[3]);
    net_note("probe leave", text);
}
template <unsigned... I>
constexpr std::array<HostFunc, sizeof...(I)> net_probe_entries(std::integer_sequence<unsigned, I...>) { return {&net_probe_entry<I>...}; }
}
void rbrew_net_probe_install(CPUState* cpu) {
    const char* list = std::getenv("RBREW_NET_PROBE");
    if (!list || !*list || !net_trace_file()) return;
    static constexpr auto entries = net_probe_entries(std::make_integer_sequence<unsigned, kNetProbes>{});
    unsigned n = 0;
    for (const char* p=list;*p;) {
        const char* end=std::strchr(p,',');if(!end)end=p+std::strlen(p);
        const char* begin=p;
        if(end-begin>=2 && begin[0]=='0' && (begin[1]=='x'||begin[1]=='X'))begin+=2;
        uint32_t address=0;const auto parsed=std::from_chars(begin,end,address,16);
        const uint32_t slot=address>>2;const char* reason=nullptr;
        if(parsed.ec!=std::errc{} || parsed.ptr!=end || begin==end || !address || (address&3))reason="invalid address";
        else if(slot>=cpu->dispatch_size || !cpu->dispatch_table[slot])reason="no dispatch entry";
        else {
            for(unsigned i=0;i<n;++i)if(s_net_probes[i].address==address)reason="duplicate";
            if(!reason && n==kNetProbes)reason="16-address limit";
        }
        if(reason) {
            char text[160];std::snprintf(text,sizeof text,"%.*s (%s)",int(end-p),p,reason);net_note("probe ignored",text);
        } else {
            s_net_probes[n]={address,cpu->dispatch_table[slot]};cpu->dispatch_table[slot]=entries[n];++n;
        }
        p=*end?end+1:end;
    }
    char text[32]; std::snprintf(text, sizeof text, "%u functions", n);
    net_note("probe installed", text);
}
// RBREW_NET_CALLS (indirect-call trace) needs a dispatcher hook that is a
// diagnostic of the old tree and is not carried over: no-ops here.
static void net_calls_arm() {}
static void net_calls_disarm() {}

static void net_packet(const char* dir, uint32_t fd, uint32_t addr_host, uint16_t port_host, const uint8_t* data, int len) {
    FILE* f = net_trace_file();
    if (!f || len < 0) return;
    char text[320]; int n = std::snprintf(text, sizeof text, "fd=%u %u.%u.%u.%u:%u len=%d ", fd, addr_host >> 24, (addr_host >> 16) & 255,
                                          (addr_host >> 8) & 255, addr_host & 255, port_host, len);
    // PIA packets (magic 32AB9864, peer-to-peer) are shown up to 128 bytes: their station locations sit past byte 32.
    const int shown = len >= 4 && data[0] == 0x32 && data[1] == 0xAB && data[2] == 0x98 && data[3] == 0x64 ? 128 : 32;
    for (int i = 0; i < len && i < shown && n < int(sizeof text) - 3; ++i) n += std::snprintf(text + n, sizeof text - n, "%02X", data[i]);
    net_note(dir, text);
}

// ---------------------------------------------------------------------------
// Scratch area for gethostbyname / getaddrinfo / inet_ntoa results.
// All offsets are relative to NET_SCRATCH_BASE in the guest arena.
// ---------------------------------------------------------------------------
static constexpr uint32_t NET_SCRATCH_BASE = 0x00100000u; // safely below game code

// hostent scratch (128 bytes)
static constexpr uint32_t HE_BASE = NET_SCRATCH_BASE + 0x000u;
static constexpr uint32_t HE_ALIASES_ARR = NET_SCRATCH_BASE + 0x020u;  // uint32[2] = {0,0}
static constexpr uint32_t HE_ADDRLIST_ARR = NET_SCRATCH_BASE + 0x028u; // uint32[2] = {addr,0}
static constexpr uint32_t HE_ADDR = NET_SCRATCH_BASE + 0x030u;         // 4-byte IP
static constexpr uint32_t HE_NAME_STR = NET_SCRATCH_BASE + 0x040u;     // hostname (64 chars)

// addrinfo scratch (up to 4 entries, each 32-byte struct + 16-byte sockaddr_in)
static constexpr uint32_t AI_BASE = NET_SCRATCH_BASE + 0x100u;
static constexpr uint32_t AI_ENTRY_STRIDE = 48u; // 32 (addrinfo) + 16 (sockaddr_in)
static constexpr uint32_t AI_MAX_ENTRIES = 4u;
static constexpr uint32_t AI_CANON_STR = NET_SCRATCH_BASE + 0x200u; // 64 chars

// inet_ntoa result buffer (cycling through 4 slots to match POSIX static behaviour)
static constexpr uint32_t INET_NTOA_BASE   = NET_SCRATCH_BASE + 0x280u;
static constexpr uint32_t INET_NTOA_SLOT   = 20u; // max "255.255.255.255\0" = 16 chars
static std::atomic<uint32_t> s_ntoa_slot{0};

// ---------------------------------------------------------------------------
// sockaddr_in guest ↔ host conversion
// ---------------------------------------------------------------------------
static void guest_to_host_sockaddr(const uint8_t* mem, uint32_t gptr, struct sockaddr_in* out) {
    memset(out, 0, sizeof(*out));
    if (!gptr) return;
    out->sin_family = (sa_family_t)((uint16_t(mem[gptr])<<8)|mem[gptr+1]);
    memcpy(&out->sin_port,         mem + gptr + 2, 2); // already network order
    memcpy(&out->sin_addr.s_addr,  mem + gptr + 4, 4); // already network order
}

static void host_to_guest_sockaddr(uint8_t* mem,uint32_t p,const sockaddr_in* in,uint32_t capacity=16) {
 uint8_t bytes[16]{};bytes[1]=uint8_t(in->sin_family);memcpy(bytes+2,&in->sin_port,2);memcpy(bytes+4,&in->sin_addr.s_addr,4);if(p)memcpy(mem+p,bytes,std::min(capacity,16u));
}

// ---------------------------------------------------------------------------
// setsockopt level/option mapping: Wii U BSD → Linux POSIX
// ---------------------------------------------------------------------------
static int map_level(int wii_level) {
    if (wii_level == 0xFFFF || wii_level == -1) return SOL_SOCKET;
    if (wii_level == 6)      return IPPROTO_TCP;
    if (wii_level == 0)      return IPPROTO_IP;
    return wii_level;
}

static int map_so_option(int wii_opt) {
    // Map BSD SOL_SOCKET option values (Wii U) to Linux equivalents
    switch (wii_opt) {
    case 0x0001: return SO_DEBUG;
    case 0x0004: return SO_REUSEADDR;
    case 0x0008: return SO_KEEPALIVE;
    case 0x0010: return SO_DONTROUTE;
    case 0x0020: return SO_BROADCAST;
    case 0x0080: return SO_LINGER;
    case 0x0100: return SO_OOBINLINE;
    case 0x1001: return SO_SNDBUF;
    case 0x1002: return SO_RCVBUF;
    case 0x1007: return SO_ERROR;
    case 0x1008: return SO_TYPE;
    default: return wii_opt;
    }
}

// ---------------------------------------------------------------------------
// select() fd_set conversion
// Wii U fd_set: big-endian uint32 bitmap (bit N = fd N, from LSB)
// Linux fd_set: same bit-numbering but stored little-endian internally.
// Since rbrew_read32 byte-swaps, we recover the logical bit values correctly.
// ---------------------------------------------------------------------------
// Guest bit N is guest descriptor N; the host set holds its host socket.
static fd_set guest_to_fdset(uint8_t* mem, uint32_t gptr, int& max_host) {
    fd_set out;
    FD_ZERO(&out);
    if (!gptr) return out;
    uint32_t bits = rbrew_read32(mem, gptr);
    for (int i = 0; i < 32; i++) {
        const HostSocket s = s_fds.get(i);
        if ((bits & (1u << i)) && s != HostSocket(INVALID_SOCKET)) {
            FD_SET(s, &out);
            if (int(s) > max_host) max_host = int(s);
        }
    }
    return out;
}

static void fdset_to_guest(uint8_t* mem, uint32_t gptr, fd_set* s) {
    if (!gptr) return;
    uint32_t bits = 0;
    for (int i = 0; i < 32; i++) {
        const HostSocket h = s_fds.get(i);
        if (h != HostSocket(INVALID_SOCKET) && FD_ISSET(h, s)) bits |= (1u << i);
    }
    rbrew_write32(mem, gptr, bits);
}

// ---------------------------------------------------------------------------
// socket_lib_init / NSSLInit — initialise the socket library
// On Windows, calls WSAStartup; on POSIX this is a no-op.
// ---------------------------------------------------------------------------
static void socket_lib_init(CPUState* cpu) {
#if defined(_WIN32)
    WSADATA wd;
    WSAStartup(MAKEWORD(2, 2), &wd);
#endif
    RBREW_LOG_INFO("nsysnet", "socket_lib_init\n");
    RET = 0;
}

static void NSSLInit(CPUState* cpu) {
    (void)cpu;
    RET = 0;
}
static void set_resolver_allocator(CPUState* cpu) {
    (void)cpu;
}

// ---------------------------------------------------------------------------
// mw_socket / socketclose
// On Windows, SOCKET is UINT_PTR but games always get small FD numbers so
// casting to int is safe (Wii U socket descriptors fit in int).
// ---------------------------------------------------------------------------
static void mw_socket(CPUState* cpu){
 const uint32_t domain=ARG0,type=ARG1,protocol=ARG2;
 if(domain!=AF_INET){net_fail(cpu,kErrFamily);return;}
 std::lock_guard lock(s_socket_io_mutex);const auto s=::socket(int(domain),int(type),int(protocol));
 if(s==HostSocket(INVALID_SOCKET)){net_result(cpu,-1);return;}
#if defined(_WIN32)
 u_long mode=1;const int rc=ioctlsocket(s,FIONBIO,&mode);
#else
 const int rc=fcntl(s,F_SETFL,fcntl(s,F_GETFL,0)|O_NONBLOCK);
#endif
 if(rc<0){const int e=SOCK_ERRNO;CLOSE_SOCKET(s);net_fail(cpu,e);return;}
 const int fd=s_fds.add(s);if(fd<0){CLOSE_SOCKET(s);net_fail(cpu,kErrFull);return;}
 auto& state=s_socket_modes[fd];state.nonblocking=false;++state.generation;
#if defined(_WIN32)
 if(type==SOCK_DGRAM){BOOL off=FALSE;DWORD bytes=0;WSAIoctl(s,SIO_UDP_CONNRESET,&off,sizeof off,nullptr,0,&bytes,nullptr,nullptr);}
#endif
 net_result(cpu,fd);
}
static void socketclose(CPUState* cpu){const uint32_t fd=ARG0;std::lock_guard lock(s_socket_io_mutex);const auto s=s_fds.remove(int(fd));if(s==HostSocket(INVALID_SOCKET)){net_fail(cpu,kErrBadFd);return;}++s_socket_modes[fd].generation;net_result(cpu,CLOSE_SOCKET(s));}
static void socketlasterr(CPUState* cpu){RET=s_guest_socket_error;}
static void bind_(CPUState* cpu){
 const uint32_t fd=ARG0,p=ARG1,len=ARG2;if(len<16||!guest_buffer(cpu,p,16)){net_fail(cpu,kErrFault);return;}
 sockaddr_in sa{};guest_to_host_sockaddr(MEM,p,&sa);if(sa.sin_family!=AF_INET){net_fail(cpu,kErrFamily);return;}
 sa.sin_addr.s_addr=htonl(net_policy().bind_address(ntohl(sa.sin_addr.s_addr)));
 std::lock_guard lock(s_socket_io_mutex);const auto s=host_socket(fd);if(s==HostSocket(INVALID_SOCKET)){net_fail(cpu,kErrBadFd);return;}
 net_result(cpu,::bind(s,reinterpret_cast<sockaddr*>(&sa),sizeof(sa)));
 if(net_trace_file()){   // requested and actual local port (peer-to-peer diagnosis, 27/10)
  sockaddr_in local{};socklen_t size=sizeof(local);::getsockname(s,reinterpret_cast<sockaddr*>(&local),&size);
  char detail[96];std::snprintf(detail,sizeof(detail),"fd=%u requested=%u bound=%u",fd,unsigned(ntohs(sa.sin_port)),unsigned(ntohs(local.sin_port)));
  net_note("bind",detail);}
}
static void connect_(CPUState* cpu){
 const uint32_t fd=ARG0,p=ARG1,len=ARG2;if(len<16||!guest_buffer(cpu,p,16)){net_fail(cpu,kErrFault);return;}
 sockaddr_in sa{};guest_to_host_sockaddr(MEM,p,&sa);if(!net_policy().allow_destination(ntohl(sa.sin_addr.s_addr))){net_fail(cpu,kErrBlocked);return;}
 std::lock_guard lock(s_socket_io_mutex);const auto s=host_socket(fd);if(s==HostSocket(INVALID_SOCKET)){net_fail(cpu,kErrBadFd);return;}
 net_result(cpu,::connect(s,reinterpret_cast<sockaddr*>(&sa),sizeof(sa)));
}
static void shutdown_(CPUState* cpu){const uint32_t fd=ARG0,how=ARG1;{std::lock_guard lock(s_socket_io_mutex);const auto s=host_socket(fd);if(s==HostSocket(INVALID_SOCKET))net_fail(cpu,kErrBadFd);else net_result(cpu,::shutdown(s,int(how)));}net_calls_disarm();}
static void getsockname_(CPUState* cpu){
 const uint32_t fd=ARG0,p=ARG1,lp=ARG2;if(!guest_buffer(cpu,lp,4)){net_fail(cpu,kErrFault);return;}
 const uint32_t cap=rbrew_read32(MEM,lp);if(!p||!guest_buffer(cpu,p,std::min(cap,16u))){net_fail(cpu,kErrFault);return;}
 std::lock_guard lock(s_socket_io_mutex);const auto s=host_socket(fd);if(s==HostSocket(INVALID_SOCKET)){net_fail(cpu,kErrBadFd);return;}
 sockaddr_in sa{};socklen_t size=sizeof sa;const int rc=::getsockname(s,reinterpret_cast<sockaddr*>(&sa),&size);net_result(cpu,rc);
 if(rc==0){host_to_guest_sockaddr(MEM,p,&sa,cap);rbrew_write32(MEM,lp,16);}
}

// ---------------------------------------------------------------------------
// send / recv / sendto / recvfrom / recvfrom_ex
// ---------------------------------------------------------------------------
static void send_(CPUState* cpu) {
    const uint32_t fd=ARG0,buffer=ARG1,length=ARG2,flags=ARG3;
    if(length>INT_MAX || !guest_buffer(cpu,buffer,length)){net_fail(cpu,kErrFault);return;}
    const int n=socket_io(fd,flags,[&](HostSocket s,int hf){return int(send(s,reinterpret_cast<const char*>(MEM+buffer),int(length),hf));});
    RET=uint32_t(n);
}
static void recv_(CPUState* cpu) {
    const uint32_t fd=ARG0,buffer=ARG1,length=ARG2,flags=ARG3;
    if(length>INT_MAX || !guest_buffer(cpu,buffer,length)){net_fail(cpu,kErrFault);return;}
    const int n=socket_io(fd,flags,[&](HostSocket s,int hf){return int(recv(s,reinterpret_cast<char*>(MEM+buffer),int(length),hf));});
    RET=uint32_t(n);
}
static void sendto_(CPUState* cpu) {
    const uint32_t fd=ARG0,buffer=ARG1,length=ARG2,flags=ARG3,address=ARG4,alen=ARG5;
    if(length>INT_MAX || !guest_buffer(cpu,buffer,length) || alen<16 || !guest_buffer(cpu,address,16)){net_fail(cpu,kErrFault);return;}
    sockaddr_in sa{};guest_to_host_sockaddr(MEM,address,&sa);
    if(sa.sin_family!=AF_INET){net_fail(cpu,kErrFamily);return;}
    const uint32_t dest=ntohl(sa.sin_addr.s_addr);
    // Online: a match host learns its players' addresses from the server's NAT traversal
    // calls, which this layer cannot read, so a datagram to any unicast address is let
    // through and that address joins the bounded, expiring peer list. Names still resolve
    // only to the announced server (DNS overrides), so no other service is reachable by name.
    if(!net_policy().allow_destination(dest) && !(net_policy().online() && net_policy().add_peer(dest))){net_packet("sendto BLOCKED",fd,dest,ntohs(sa.sin_port),MEM+buffer,int(length));net_fail(cpu,kErrBlocked);return;}
    const int n=socket_io(fd,flags,[&](HostSocket s,int hf){return int(sendto(s,reinterpret_cast<const char*>(MEM+buffer),int(length),hf,reinterpret_cast<sockaddr*>(&sa),sizeof(sa)));});
    net_packet("sendto",fd,dest,ntohs(sa.sin_port),MEM+buffer,n);RET=uint32_t(n);
}
static void recvfrom_(CPUState* cpu) {
    const uint32_t fd=ARG0,buffer=ARG1,length=ARG2,flags=ARG3,address=ARG4,alen=ARG5;
    uint32_t capacity=0;
    if(length>INT_MAX || !guest_buffer(cpu,buffer,length) ||
       (address && (!guest_buffer(cpu,alen,4) || !guest_buffer(cpu,address,std::min(rbrew_read32(MEM,alen),16u))))){net_fail(cpu,kErrFault);return;}
    if(address)capacity=rbrew_read32(MEM,alen);
    sockaddr_in sa{};socklen_t size=sizeof(sa);
    // RBREW_STRICT_UDP_IN=1 (off by default until it is tested between two real networks): online,
    // a datagram is delivered to the game only when its source is this PC, the announced server or a
    // peer this PC has already sent to (the peer list that sendto fills). Anyone else is dropped,
    // so a stranger who guesses a port cannot feed bytes to the game's network code.
    static const bool strict_in=[]{const char* v=std::getenv("RBREW_STRICT_UDP_IN");return v && v[0]=='1';}();
    const int n=socket_io(fd,flags,[&](HostSocket s,int hf){
        for(;;){
            size=sizeof(sa);
            const int r=int(recvfrom(s,reinterpret_cast<char*>(MEM+buffer),int(length),hf,reinterpret_cast<sockaddr*>(&sa),&size));
            if(r>=0 && strict_in && net_policy().online() && !net_policy().allow_destination(ntohl(sa.sin_addr.s_addr))){
                net_packet("recvfrom DROPPED",fd,ntohl(sa.sin_addr.s_addr),ntohs(sa.sin_port),MEM+buffer,r);
                continue;
            }
            return r;
        }
    });
    if(n>=0) {
        if(address){host_to_guest_sockaddr(MEM,address,&sa,capacity);rbrew_write32(MEM,alen,16);}
        net_packet("recvfrom",fd,ntohl(sa.sin_addr.s_addr),ntohs(sa.sin_port),MEM+buffer,n);net_calls_arm();
    }
    RET=uint32_t(n);
}


// recvfrom_ex(fd, buf, len, flags, from, fromlen, extra, extra_len): PIA's
// Socket::RecvFrom (02C48B50, NAT check and peer traffic) passes flags 0x60 =
// MSG_DONTWAIT | 0x40, the extension bit that asks for `extra`, with extra_len 1.
// Plain recvfrom refused 0x40 (EINVAL) so the game never saw
// a NAT-check reply. The extra bytes are cleared (hypothesis: no extended info).
static void recvfrom_ex(CPUState* cpu) {
    const uint32_t extra=ARG6,extra_len=std::min<uint32_t>(ARG7,64u);
    cpu->r[6]&=~0x40u;
    recvfrom_(cpu);
    if(int32_t(RET)>=0 && extra && guest_buffer(cpu,extra,extra_len)) std::memset(MEM+extra,0,extra_len);
}

// sendto_multi: not emulated; return 0
static void sendto_multi(CPUState* cpu) {
    (void)cpu;
    RET = 0;
}

// ---------------------------------------------------------------------------
// select
// ARG0=nfds, ARG1=readfds*, ARG2=writefds*, ARG3=exceptfds*, ARG4=timeval*
// Wii U timeval: +0 int32 sec (BE), +4 int32 usec (BE)
// ---------------------------------------------------------------------------
static void select_(CPUState* cpu) {
    const int nfds=int(ARG0);const uint32_t rp=ARG1,wp=ARG2,ep=ARG3,tp=ARG4;
    if(nfds<0 || nfds>32){net_fail(cpu,kErrInvalid);return;}
    for(uint32_t p:{rp,wp,ep})if(p && !guest_buffer(cpu,p,4)){net_fail(cpu,kErrFault);return;}
    if(tp && !guest_buffer(cpu,tp,8)){net_fail(cpu,kErrFault);return;}
    int64_t timeout=-1;
    if(tp){const int32_t sec=int32_t(rbrew_read32(MEM,tp)),us=int32_t(rbrew_read32(MEM,tp+4));
        if(sec<0 || us<0 || us>=1000000){net_fail(cpu,kErrInvalid);return;}timeout=int64_t(sec)*1000000+us;}
    const uint32_t mask=nfds==32?~0u:(1u<<nfds)-1;
    const uint32_t rbits=rp?rbrew_read32(MEM,rp)&mask:0,wbits=wp?rbrew_read32(MEM,wp)&mask:0,ebits=ep?rbrew_read32(MEM,ep)&mask:0;
    const uint32_t all=rbits|wbits|ebits;
    std::array<uint64_t,32> generations{};
    {std::lock_guard lock(s_socket_io_mutex);for(int i=0;i<nfds;++i)if(all&(1u<<i)){
        if(s_fds.get(i)==HostSocket(INVALID_SOCKET)){net_fail(cpu,kErrBadFd);return;}generations[i]=s_socket_modes[i].generation;}}
    const auto started=std::chrono::steady_clock::now();
    for(;;) {
        fd_set r,w,e;FD_ZERO(&r);FD_ZERO(&w);FD_ZERO(&e);uint32_t rout=0,wout=0,eout=0;int rc=0,max_host=-1;
        {
            std::lock_guard lock(s_socket_io_mutex);
            for(int i=0;i<nfds;++i)if(all&(1u<<i)){
                const auto h=s_fds.get(i);
                if(h==HostSocket(INVALID_SOCKET) || generations[i]!=s_socket_modes[i].generation){net_fail(cpu,kErrBadFd);return;}
#if !defined(_WIN32)
                if(h>=FD_SETSIZE){net_fail(cpu,kErrBadFd);return;}
                max_host=std::max(max_host,int(h));
#endif
                if(rbits&(1u<<i))FD_SET(h,&r);if(wbits&(1u<<i))FD_SET(h,&w);if(ebits&(1u<<i))FD_SET(h,&e);
            }
            if(all){timeval zero{};rc=::select(max_host+1,rp?&r:nullptr,wp?&w:nullptr,ep?&e:nullptr,&zero);}
            if(rc<0){net_result(cpu,rc);return;}
            if(rc>0)for(int i=0;i<nfds;++i)if(all&(1u<<i)){
                const auto h=s_fds.get(i);if(FD_ISSET(h,&r))rout|=1u<<i;if(FD_ISSET(h,&w))wout|=1u<<i;if(FD_ISSET(h,&e))eout|=1u<<i;
            }
        }
        const bool expired=timeout>=0 && std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-started).count()>=timeout;
        if(rc>0 || expired || (!all && timeout<0)) {
            if(rp)rbrew_write32(MEM,rp,rout);if(wp)rbrew_write32(MEM,wp,wout);if(ep)rbrew_write32(MEM,ep,eout);net_result(cpu,rc);return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}


// ---------------------------------------------------------------------------
// setsockopt
// ARG0=fd, ARG1=level, ARG2=optname, ARG3=optval_ptr, ARG4=optlen
// ---------------------------------------------------------------------------
static void setsockopt_(CPUState* cpu) {
    const uint32_t fd=ARG0,opt=ARG2,vp=ARG3,vl=ARG4;const int level=map_level(int(ARG1));
    std::lock_guard lock(s_socket_io_mutex);const HostSocket s=host_socket(fd);
    if(s==HostSocket(INVALID_SOCKET)){net_fail(cpu,kErrBadFd);return;}
    if(level==SOL_SOCKET && opt>=0x1014 && opt<=0x1016) {
        if((opt==0x1016 && (vl!=4 || !guest_buffer(cpu,vp,4))) ||
           (opt!=0x1016 && vl!=0 && (vl!=4 || !guest_buffer(cpu,vp,4)))){net_fail(cpu,kErrInvalid);return;}
        s_socket_modes[fd].nonblocking=opt==0x1014 || (opt==0x1016 && rbrew_read32(MEM,vp)!=0);
        net_result(cpu,0);return;
    }
    if(!guest_buffer(cpu,vp,vl) || (vl!=4 && !(level==SOL_SOCKET && opt==0x80 && vl==8))){net_fail(cpu,kErrInvalid);return;}
    const int option=level==SOL_SOCKET?map_so_option(int(opt)):int(opt);
    int value=int(rbrew_read32(MEM,vp));
    if(level==SOL_SOCKET && opt==0x80) {
        const uint32_t seconds=rbrew_read32(MEM,vp+4);
        if(seconds>65535){net_fail(cpu,kErrInvalid);return;}
        linger linger_value{};linger_value.l_onoff=value!=0;linger_value.l_linger=decltype(linger_value.l_linger)(seconds);
        net_result(cpu,::setsockopt(s,level,option,reinterpret_cast<const char*>(&linger_value),sizeof(linger_value)));
    } else net_result(cpu,::setsockopt(s,level,option,reinterpret_cast<const char*>(&value),sizeof(value)));
}


// ---------------------------------------------------------------------------
// Byte-order functions: on Wii U (big-endian) these are all identity ops.
// The guest calls them with values already in "host" (= network) byte order.
// ---------------------------------------------------------------------------
static void htonl_(CPUState* cpu) {
    RET = ARG0;
}
static void htons_(CPUState* cpu) {
    RET = ARG0 & 0xFFFFu;
}
static void ntohl_(CPUState* cpu) {
    RET = ARG0;
}
static void ntohs_(CPUState* cpu) {
    RET = ARG0 & 0xFFFFu;
}

// ---------------------------------------------------------------------------
// inet_ntoa(in_addr)
// ARG0 = uint32 IP address (network byte order, as stored in big-endian guest)
// Returns guest ptr to a "255.255.255.255\0" string in the scratch area.
// ---------------------------------------------------------------------------
static void inet_ntoa_(CPUState* cpu) {
    uint32_t slot = s_ntoa_slot.fetch_add(1) & 3u;
    uint32_t gptr = INET_NTOA_BASE + slot * INET_NTOA_SLOT;

    // struct in_addr is passed by reference (GHS ABI, as in Cemu): ARG0 points
    // at the big-endian address. Formatting ARG0 itself put a stack address
    // ("51.1.49.16") into the station URL NEX registers.
    if (!rbrew_memory_accessible(MEM, ARG0, 4u)) { RET = 0; return; }
    uint32_t addr_be = rbrew_read32(MEM, ARG0);
    // Extract octets from big-endian representation
    uint8_t b0 = (uint8_t)(addr_be >> 24);
    uint8_t b1 = (uint8_t)(addr_be >> 16);
    uint8_t b2 = (uint8_t)(addr_be >> 8);
    uint8_t b3 = (uint8_t)(addr_be);

    char buf[20];
    int len = snprintf(buf, sizeof(buf), "%u.%u.%u.%u", b0, b1, b2, b3);
    if (len > 0 && rbrew_memory_accessible(MEM, gptr, uint64_t(len) + 1u)) {
        memcpy(MEM + gptr, buf, (size_t)len + 1u);
        RET = gptr;
    } else {
        RET = 0;
    }
}

// ---------------------------------------------------------------------------
// inet_pton(family, src_str_ptr, dst_ptr)
// ARG0=AF_INET, ARG1=src_str (guest ptr), ARG2=dst (guest ptr, 4 bytes for AF_INET)
// ---------------------------------------------------------------------------
static void inet_pton_(CPUState* cpu) {
    if (ARG0 != 2u) {
        RET = 0;
        return;
    } // only AF_INET supported
    if (!ARG1 || !ARG2) {
        RET = 0;
        return;
    }

    // Read the string from guest memory (up to 64 chars), bounded to guest memory (B-01).
    char buf[64] = {};
    if (!guest_string(cpu, ARG1, buf, sizeof buf)) { RET = 0; return; }

    struct in_addr addr;
    int rc = inet_pton(AF_INET, buf, &addr);
    if (rc == 1 && rbrew_memory_accessible(MEM, ARG2, 4u)) {
        memcpy(MEM + ARG2, &addr.s_addr, 4); // network-order bytes
    }
    RET = (uint32_t)rc; // 1=success, 0=invalid, -1=unsupported family
}

// ---------------------------------------------------------------------------
// gethostbyname — write result into NET_SCRATCH hostent area
// Returns guest ptr to the hostent struct, or 0 on failure.
// ---------------------------------------------------------------------------
static void gethostbyname_(CPUState* cpu) {
    if (!ARG0) {
        RET = 0;
        return;
    }

    // Read hostname from guest memory, bounded to guest memory (B-01)
    char hostname[256] = {};
    if (!guest_string(cpu, ARG0, hostname, sizeof hostname)) { RET = 0; return; }

    // Confinement: no real DNS; only RBREW_DNS_OVERRIDE names (or a loopback literal).
    const auto resolved = net_policy().resolve(hostname);
    net_note(resolved ? "dns gethostbyname" : "dns gethostbyname BLOCKED", hostname);
    if (!resolved) { RET = 0; return; }

    // Write h_name string
    size_t nlen = strlen(hostname);
    if (nlen > 63) nlen = 63;
    memcpy(MEM + HE_NAME_STR, hostname, nlen);
    MEM[HE_NAME_STR + nlen] = 0;

    // Write IP address (network order)
    const uint32_t addr_be = htonl(*resolved);
    memcpy(MEM + HE_ADDR, &addr_be, 4);

    // h_aliases array: [0x00000000] (no aliases)
    rbrew_write32(MEM, HE_ALIASES_ARR, 0u);

    // h_addr_list array: [HE_ADDR, NULL]
    rbrew_write32(MEM, HE_ADDRLIST_ARR + 0u, HE_ADDR);
    rbrew_write32(MEM, HE_ADDRLIST_ARR + 4u, 0u);

    // hostent struct
    rbrew_write32(MEM, HE_BASE + 0x00u, HE_NAME_STR);     // h_name
    rbrew_write32(MEM, HE_BASE + 0x04u, HE_ALIASES_ARR);  // h_aliases
    rbrew_write32(MEM, HE_BASE + 0x08u, 2u);              // h_addrtype = AF_INET
    rbrew_write32(MEM, HE_BASE + 0x0Cu, 4u);              // h_length
    rbrew_write32(MEM, HE_BASE + 0x10u, HE_ADDRLIST_ARR); // h_addr_list

    RBREW_LOG_INFO("nsysnet", "gethostbyname(\"%s\") → %u.%u.%u.%u\n", hostname, MEM[HE_ADDR], MEM[HE_ADDR + 1],
                   MEM[HE_ADDR + 2], MEM[HE_ADDR + 3]);

    RET = HE_BASE;
}

// ---------------------------------------------------------------------------
// getaddrinfo / freeaddrinfo
// ARG0=hostname_ptr, ARG1=servname_ptr, ARG2=hints_ptr, ARG3=res_ptr (addrinfo**)
//
// Wii U addrinfo struct (big-endian):
//   +0x00: int32  ai_flags
//   +0x04: int32  ai_family
//   +0x08: int32  ai_socktype
//   +0x0C: int32  ai_protocol
//   +0x10: uint32 ai_addrlen
//   +0x14: uint32 ai_canonname (guest ptr)
//   +0x18: uint32 ai_addr     (guest ptr to sockaddr_in)
//   +0x1C: uint32 ai_next     (guest ptr)
// ---------------------------------------------------------------------------
static void getaddrinfo_(CPUState* cpu) {
    if (!ARG0 && !ARG1) {
        RET = (uint32_t)-1;
        return;
    }

    // Read hostname and service, bounded to guest memory (B-01)
    char hostname[256] = {};
    if (ARG0 && !guest_string(cpu, ARG0, hostname, sizeof hostname)) { RET = 8u; return; }
    char servname[64] = {};
    if (ARG1 && !guest_string(cpu, ARG1, servname, sizeof servname)) { RET = 8u; return; }

    // Read hints (ai_family and ai_socktype), only when the struct is readable (B-01).
    int hint_family = 0, hint_socktype = SOCK_STREAM;
    if (ARG2 && guest_buffer(cpu, ARG2, 12u)) {
        hint_family = int(rbrew_read32(MEM, ARG2 + 0x04u));
        hint_socktype = int(rbrew_read32(MEM, ARG2 + 0x08u));
    }
    // B-06: this port only has an IPv4 socket stack; asking for another family (AF_INET6,
    // 10 on Wii U) must fail instead of silently returning an IPv4 entry. 5 = EAI_FAMILY
    // in the BSD numbering this function already uses (8 = EAI_NONAME below).
    if (hint_family != 0 && hint_family != 2) { RET = 5u; return; }

    // Confinement: no real DNS. A named host resolves only through
    // RBREW_DNS_OVERRIDE (or a loopback literal); a null host (passive bind)
    // is loopback. Numeric services only; one AF_INET entry.
    const auto resolved = *hostname ? net_policy().resolve(hostname) : std::optional<uint32_t>(rbrew_net::kLoopback);
    net_note(resolved ? "dns getaddrinfo" : "dns getaddrinfo BLOCKED", *hostname ? hostname : "(passive)");
    if (!resolved) { RET = 8u; return; }   // EAI_NONAME (BSD numbering)

    const uint32_t count = 1;
    const uint32_t entry_base = AI_BASE, sa_base = AI_BASE + 32u;   // sockaddr_in follows the addrinfo
    struct sockaddr_in sin = {};
    sin.sin_family = AF_INET;
    sin.sin_port = htons(uint16_t(std::strtoul(servname, nullptr, 10)));
    sin.sin_addr.s_addr = htonl(*resolved);
    host_to_guest_sockaddr(MEM, sa_base, &sin);
    const size_t cnlen = std::min<size_t>(strlen(hostname), 63);
    memcpy(MEM + AI_CANON_STR, hostname, cnlen);
    MEM[AI_CANON_STR + cnlen] = 0;
    const int socktype = hint_socktype ? hint_socktype : SOCK_STREAM;
    rbrew_write32(MEM, entry_base + 0x00u, 0u);             // ai_flags
    rbrew_write32(MEM, entry_base + 0x04u, 2u);             // ai_family = AF_INET
    rbrew_write32(MEM, entry_base + 0x08u, (uint32_t)socktype);
    rbrew_write32(MEM, entry_base + 0x0Cu, socktype == SOCK_DGRAM ? (uint32_t)IPPROTO_UDP : (uint32_t)IPPROTO_TCP);
    rbrew_write32(MEM, entry_base + 0x10u, 16u);            // ai_addrlen
    rbrew_write32(MEM, entry_base + 0x14u, AI_CANON_STR);   // ai_canonname
    rbrew_write32(MEM, entry_base + 0x18u, sa_base);        // ai_addr
    rbrew_write32(MEM, entry_base + 0x1Cu, 0u);             // ai_next

    // Write *res = first entry
    if (ARG3) rbrew_write32(MEM, ARG3, AI_BASE);

    RBREW_LOG_INFO("nsysnet", "getaddrinfo(\"%s\",\"%s\") → %u entr%s\n", hostname, servname, count,
                   count == 1 ? "y" : "ies");
    RET = 0;
}

// freeaddrinfo — results are in a static scratch area, nothing to free
static void freeaddrinfo_(CPUState* cpu) {
    (void)cpu;
}

// ---------------------------------------------------------------------------
// nn_ac (network availability) — always report connected
// ---------------------------------------------------------------------------
static void ac_Initialize(CPUState* cpu) {
    (void)cpu;
    RET = 0;
}
// Connect() reported success while IsApplicationConnected() reported offline.
// Gambit (02A7B5A4) then started its NEX/Pia online login, found no server
// and the boot loading screen waited for the game's 30 s online timeout. Like a
// console without internet, Connect now fails (sign bit set; the description
// value is the port's choice) and the game boots offline at once.
// RBREW_AC_CONNECT=ok restores the old success result.
// Online mode (online_server=..., runtime/online_identity.h) connects; the
// confined lab network is loopback 127.0.0.1/8.
static constexpr uint32_t kAcResultNotConnected = 0xA0D1F400u;
static bool ac_online() { return rbrew_online::config().online; }
static void ac_Connect(CPUState* cpu) {
    static const bool ok = [] { const char* v = std::getenv("RBREW_AC_CONNECT"); return v && !strcmp(v, "ok"); }();
    RET = ok || ac_online() ? 0u : kAcResultNotConnected;
}
static void ac_Close(CPUState* cpu)                    { (void)cpu; RET = 0; }
static void ac_GetLastErrorCode(CPUState* cpu)         { if (ARG0) rbrew_write32(MEM, ARG0, 0u); RET = 0; }
// The address the game announces as its own (its private station URL). The lab network is
// loopback. Public mode: this PC's address on the interface that reaches the server, so two
// players behind one router can reach each other on the LAN (PIA turns away a newcomer with
// the host's public IP and no distinct private address). Found by connecting a UDP socket
// to the server, which sends nothing; loopback if that fails.
static uint32_t assigned_address() {
    static const uint32_t address = [] {
        if (!net_policy().online()) return rbrew_net::kLoopback;
        uint32_t server = 0;
        if (!rbrew_net::Policy::parse_ipv4(rbrew_online::config().host, server)) return rbrew_net::kLoopback;
#if defined(_WIN32)
        WSADATA wd; WSAStartup(MAKEWORD(2,2), &wd);
#endif
        uint32_t found = rbrew_net::kLoopback;
        const auto s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        sockaddr_in to{}; to.sin_family = AF_INET; to.sin_port = htons(rbrew_online::config().port); to.sin_addr.s_addr = htonl(server);
        sockaddr_in local{}; socklen_t size = sizeof local;
        if (::connect(s, reinterpret_cast<sockaddr*>(&to), sizeof to) == 0 &&
            ::getsockname(s, reinterpret_cast<sockaddr*>(&local), &size) == 0 && local.sin_addr.s_addr)
            found = ntohl(local.sin_addr.s_addr);
        CLOSE_SOCKET(s);
        return found;
    }();
    return address;
}
static void ac_GetAssignedAddress(CPUState* cpu)       { if (ARG0) rbrew_write32(MEM, ARG0, ac_online() ? assigned_address() : 0u); RET = 0; }
static void ac_GetAssignedSubnet(CPUState* cpu)        { if (ARG0) rbrew_write32(MEM, ARG0, !ac_online() ? 0u : rbrew_net::Policy::is_loopback(assigned_address()) ? 0xFF000000u : 0xFFFFFF00u); RET = 0; }

// IsApplicationConnected(bool* result): false offline (prevents lobby network
// hangs), true in online mode.
// With RBREW_NET_TRACE: the inputs of the game's "online" test (028C2248, the
// shops and Spike: Welcome or Offline), logged when they change. Rev. 288
// globals: NetworkTime 0x101DD324 (+0x15D valid), VSSetting 0x101E0AA8 (+0x18
// parsed, +0x19 SpotPass done, +0x2C40 current phase, +0x2C48 its Turf maps).
static void net_online_inputs(CPUState* cpu) {
    if (!net_trace_file()) return;
    const auto range = [&](uint64_t p,uint32_t size){return p && p+size<=cpu->guest_memory_size && rbrew_memory_accessible(MEM,uint32_t(p),size);};
    const auto object = [&](uint32_t global) { return range(global,4) ? rbrew_read32(MEM, global) : 0u; };
    const uint32_t net_time = object(0x101DD324u), vs = object(0x101E0AA8u);
    const auto byte = [&](uint32_t base, uint32_t off) { return base && range(uint64_t(base)+off,1) ? int(MEM[base + off]) : -1; };
    const auto word = [&](uint32_t base, uint32_t off) { return base && range(uint64_t(base)+off,4) ? int32_t(rbrew_read32(MEM, base + off)) : -1; };
    // Then: object(0x101E567C)+0x268 -> +0x19 must be 0; the save's festival
    // state (save manager 0x101E675C -> +0xC -> +0x10 + 0x1C) == 1 needs
    // Festival 0x101E0A60 loaded (+0x1E set and +0x1C file bits).
    const uint32_t other = word(object(0x101E567Cu), 0x268) < 0 ? 0 : uint32_t(word(object(0x101E567Cu), 0x268));
    const int32_t save_block = word(object(0x101E675Cu), 0xC);
    const int fest_state = save_block > 0 ? word(uint32_t(save_block), 0x10 + 0x1C) : -1;
    const uint32_t fest = object(0x101E0A60u);
    const int fest_bits = fest && range(uint64_t(fest)+0x1C,2) ? int(rbrew_read16(MEM, fest + 0x1C)) : -1;
    char text[220];
    std::snprintf(text, sizeof text, "nettime=%d vs_parsed=%d vs_done=%d phase=%d turf_maps=%d other19=%d save_fest=%d fest_on=%d fest_bits=0x%X",
                  byte(net_time, 0x15D), byte(vs, 0x18), byte(vs, 0x19), word(vs, 0x2C40), word(vs, 0x2C48),
                  byte(other, 0x19), fest_state, byte(fest, 0x1E), unsigned(fest_bits));
    static std::string last;
    std::lock_guard guard(s_trace_lock);
    if (last == text) return;
    last = text;
    std::fprintf(net_trace_file(), "%9.3f %-24s %s\n", net_trace_time(), "online inputs", text);
    std::fflush(net_trace_file());
}

static void ac_IsApplicationConnected(CPUState* cpu) {
    if (ARG0 && rbrew_memory_accessible(MEM,ARG0,1u))
        MEM[ARG0] = ac_online() ? 1u : 0u;
    net_online_inputs(cpu);
    RET = 0;
}

// ---------------------------------------------------------------------------
// nlibcurl — game links curl for HTTPS leaderboards / telemetry.
// Route through host libcurl (already in CMakeLists); just let the game call
// it directly via its own thunks.  For the PLT stubs we only need no-ops for
// the two global lifecycle calls.
// ---------------------------------------------------------------------------
static void curl_global_init_mem_(CPUState* cpu) {
    (void)cpu;
    RET = 0;
} // CURLE_OK
static void curl_global_cleanup_(CPUState* cpu) {
    (void)cpu;
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------
#include "../stubs/stubs_addrs.h"

// RBREW_DISPATCH_RING (rebrewu_runtime.cpp): writes the recent guest virtual calls; no-op when unset.
// Declared here, not in rebrewu_runtime.h, which every generated source includes.
extern "C" void rbrew_dispatch_ring_dump();

// RBREW_NET_TRACE=<file>: every socket/nn_ac call with its arguments, result
// and host error, plus a timestamp, for diagnosing network waits.
static void net_trace(const char* name, CPUState* cpu, uint32_t a0, uint32_t a1, uint32_t a2) {
    FILE* const file = net_trace_file();   // shared with net_note/net_packet
    if (!file) return;
    const int error = SOCK_ERRNO;
    if (RET == 0xFFFFFFFFu && rbrew_net::socket_error(error) == 6) return;   // empty polls of non-blocking sockets
    std::lock_guard guard(s_trace_lock);
    std::fprintf(file, "%9.3f %-24s %08X %08X %08X -> %08X err=%d\n",
                 net_trace_time(), name, a0, a1, a2, RET, error);
    // Who gave up on the socket: guest return addresses from the r1 back chain
    // (each frame's caller LR is saved at back_chain+4).
    if (!strcmp(name, "shutdown_") || !strcmp(name, "socketclose")) {
        std::fprintf(file, "%9s   stack lr=%08X", "", cpu->lr);
        uint32_t frame = cpu->r[1];
        for (int depth = 0; depth < 16 && frame && rbrew_memory_accessible(MEM, frame, 8u); ++depth) {
            frame = rbrew_read32(MEM, frame);
            if (!frame || !rbrew_memory_accessible(MEM, frame, 8u)) break;
            std::fprintf(file, " %08X", rbrew_read32(MEM, frame + 4));
        }
        std::fputc('\n', file);
        if (!strcmp(name, "socketclose")) rbrew_dispatch_ring_dump();
    }
    std::fflush(file);
}
// With RBREW_NET_TRACE: who raises the "communication error" that ends a duo
// session (31/10). Rev. 288 network error manager = *(0x101E80A8)+0x18:
// +0x00 state (1 error, >1 fatal: the vtable+0x1C/+0x24 tests of cSilentError
// 027B09A4), +0x04/+0x08/+0x0C detail/category/code, +0x10..+0x1C pending
// copy, +0x5C/+0x60 session error (setter 02B38E98), +0xB0/+0xB4 NEX result.
// Logged: each call of the setters 02B38E98 (code) and 02B38ED4 (detail,
// category, code) with the guest stack, each change of those fields seen by
// the poll 02B38D70, and the entry of cSilentError. Hooks: agmp-v288.hooks.
namespace err_probe {
constexpr uint32_t kSetCode = 0x02B38E98u, kSetDetail = 0x02B38ED4u, kPoll = 0x02B38D70u, kSilentError = 0x027B09A4u;
uint32_t word(CPUState* cpu, uint32_t addr) { return rbrew_memory_accessible(MEM, addr, 4u) ? rbrew_read32(MEM, addr) : 0xFFFFFFFFu; }
uint32_t manager(CPUState* cpu) {
    const uint32_t net = word(cpu, 0x101E80A8u), m = net && net != 0xFFFFFFFFu ? word(cpu, net + 0x18u) : 0u;
    return m && m != 0xFFFFFFFFu && rbrew_memory_accessible(MEM, m, 0xC0u) ? m : 0u;
}
std::string fields(CPUState* cpu) {
    const uint32_t m = manager(cpu);
    if (!m) return "manager=?";
    char text[200];
    std::snprintf(text, sizeof text, "state=%X/%X/%X/%X pending=%X/%X/%X/%X session=%u:%X nex=%u:%08X",
                  word(cpu, m), word(cpu, m + 4), word(cpu, m + 8), word(cpu, m + 0xC),
                  word(cpu, m + 0x10), word(cpu, m + 0x14), word(cpu, m + 0x18), word(cpu, m + 0x1C),
                  rbrew_read8(MEM, m + 0x5C), word(cpu, m + 0x60), rbrew_read8(MEM, m + 0xB0), word(cpu, m + 0xB4));
    return text;
}
// Guest return addresses: lr, then each frame's saved LR at back_chain+4.
std::string stack(CPUState* cpu) {
    std::string text = " stack";
    char item[12];
    std::snprintf(item, sizeof item, " %08X", cpu->lr);
    text += item;
    for (uint32_t frame = cpu->r[1], depth = 0; depth < 16; ++depth) {
        frame = word(cpu, frame);
        if (!frame || frame == 0xFFFFFFFFu || !rbrew_memory_accessible(MEM, frame + 4, 4u)) break;
        std::snprintf(item, sizeof item, " %08X", rbrew_read32(MEM, frame + 4));
        text += item;
    }
    return text;
}
void set_code(CPUState* cpu, uint32_t, HostFunc original) {
    char head[48];
    std::snprintf(head, sizeof head, "code=%X ->", word(cpu, ARG1));
    const std::string where = stack(cpu);
    original(cpu);
    net_note("err set code", (head + (" " + fields(cpu)) + where).c_str());
}
void set_detail(CPUState* cpu, uint32_t, HostFunc original) {
    char head[64];
    std::snprintf(head, sizeof head, "detail=%X category=%X code=%X ->", word(cpu, ARG1), word(cpu, ARG2), word(cpu, cpu->r[6]));
    const std::string where = stack(cpu);
    original(cpu);
    net_note("err set detail", (head + (" " + fields(cpu)) + where).c_str());
}
void poll(CPUState* cpu, uint32_t, HostFunc original) {
    original(cpu);
    static std::mutex lock;
    static std::string last;
    std::string now = fields(cpu);
    std::lock_guard guard(lock);
    if (now == last) return;
    last = now;
    net_note("err changed", now.c_str());
}
void silent_error(CPUState* cpu, uint32_t, HostFunc original) {
    net_note("err cSilentError", (fields(cpu) + stack(cpu)).c_str());
    original(cpu);
}
void install() {
    if (!net_trace_file()) return;
    const bool ok = rbrew_set_guest_hook(kSetCode, set_code) && rbrew_set_guest_hook(kSetDetail, set_detail) &&
                    rbrew_set_guest_hook(kPoll, poll) && rbrew_set_guest_hook(kSilentError, silent_error);
    net_note("err probe", ok ? "installed" : "hook table full");
}
}

// RBREW_DUO_NO_AFK=1 (set by the duo test launchers only): two games on one PC, so the
// window not being played never moves and the match's idle detector 02753D9C (rev. 288)
// raises error 0x12 ("A communication error occurred"). Skipping it keeps both in the match.
// Players never get it: the game's own AFK kick stays on everywhere else.
namespace idle_kick {
void skip(CPUState*, uint32_t, HostFunc) {}
void install() {
    const char* v = std::getenv("RBREW_DUO_NO_AFK");
    if (!v || std::strcmp(v, "1") != 0) return;
    net_note("duo test", rbrew_set_guest_hook(0x02753D9Cu, skip) ? "idle kick off" : "hook table full");
}
}

#define NET_REG(addr, fn) rbrew_register_func(cpu, addr, [](CPUState* cpu) { \
        const uint32_t a0 = ARG0, a1 = ARG1, a2 = ARG2; fn(cpu); net_trace(#fn, cpu, a0, a1, a2); })

void nsysnet_register(CPUState* cpu) {
    err_probe::install();
    idle_kick::install();
    // nsysnet core
    NET_REG(ADDR_socket_lib_init, socket_lib_init);
    NET_REG(ADDR_NSSLInit, NSSLInit);
    NET_REG(ADDR_set_resolver_allocator, set_resolver_allocator);
    NET_REG(ADDR_mw_socket, mw_socket);
    NET_REG(ADDR_socketclose, socketclose);
    NET_REG(ADDR_socketlasterr, socketlasterr);
    NET_REG(ADDR_bind, bind_);
    NET_REG(ADDR_shutdown, shutdown_);
    NET_REG(ADDR_getsockname, getsockname_);
    NET_REG(ADDR_send, send_);
    NET_REG(ADDR_recv, recv_);
    NET_REG(ADDR_sendto, sendto_);
    NET_REG(ADDR_recvfrom, recvfrom_);
    NET_REG(ADDR_recvfrom_ex, recvfrom_ex);
    NET_REG(ADDR_sendto_multi, sendto_multi);
    NET_REG(ADDR_select, select_);
    NET_REG(ADDR_setsockopt, setsockopt_);
    // Note: connect/accept/listen are not in this game's PLT
    NET_REG(ADDR_htonl, htonl_);
    NET_REG(ADDR_htons, htons_);
    NET_REG(ADDR_ntohl, ntohl_);
    NET_REG(ADDR_ntohs, ntohs_);
    NET_REG(ADDR_inet_ntoa, inet_ntoa_);
    NET_REG(ADDR_inet_pton, inet_pton_);
    NET_REG(ADDR_gethostbyname, gethostbyname_);
    NET_REG(ADDR_getaddrinfo, getaddrinfo_);
    NET_REG(ADDR_freeaddrinfo, freeaddrinfo_);

    // nn_ac (C++ mangled names → use addresses directly)
    NET_REG(ADDR_Initialize__Q2_2nn2acFv, ac_Initialize);
    NET_REG(ADDR_Connect__Q2_2nn2acFv, ac_Connect);
    NET_REG(ADDR_Close__Q2_2nn2acFv, ac_Close);
    NET_REG(ADDR_GetLastErrorCode__Q2_2nn2acFPUi, ac_GetLastErrorCode);
    NET_REG(ADDR_GetAssignedAddress__Q2_2nn2acFPUl, ac_GetAssignedAddress);
    NET_REG(ADDR_GetAssignedSubnet__Q2_2nn2acFPUl, ac_GetAssignedSubnet);
    NET_REG(ADDR_IsApplicationConnected__Q2_2nn2acFPb, ac_IsApplicationConnected);

    // nlibcurl lifecycle
    NET_REG(ADDR_curl_global_init_mem, curl_global_init_mem_);
    NET_REG(ADDR_curl_global_cleanup, curl_global_cleanup_);
}





