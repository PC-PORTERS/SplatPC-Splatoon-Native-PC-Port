#pragma once
#include <cstdint>
#if defined(_WIN32)
#include <winsock2.h>
#else
#include <cerrno>
#include <sys/socket.h>
#endif
namespace rbrew_net {
// nsysnet numbering confirmed by devkitPro/wut, wut_socket_common.c.
inline uint32_t socket_error(int value) {
    if(!value)return 0;
#if defined(_WIN32)
#define MAP(posix,win,guest) if(value==win)return guest
#else
#define MAP(posix,win,guest) if(value==posix)return guest
#endif
    MAP(ENOBUFS,WSAENOBUFS,1);MAP(ETIMEDOUT,WSAETIMEDOUT,2);MAP(EISCONN,WSAEISCONN,3);
    MAP(EOPNOTSUPP,WSAEOPNOTSUPP,4);MAP(ECONNABORTED,WSAECONNABORTED,5);
    MAP(EWOULDBLOCK,WSAEWOULDBLOCK,6);MAP(ECONNREFUSED,WSAECONNREFUSED,7);
    MAP(ECONNRESET,WSAECONNRESET,8);MAP(ENOTCONN,WSAENOTCONN,9);MAP(EALREADY,WSAEALREADY,10);
    MAP(EINVAL,WSAEINVAL,11);MAP(EMSGSIZE,WSAEMSGSIZE,12);MAP(EDESTADDRREQ,WSAEDESTADDRREQ,14);
    MAP(ESHUTDOWN,WSAESHUTDOWN,15);MAP(ENOPROTOOPT,WSAENOPROTOOPT,16);
    MAP(EADDRNOTAVAIL,WSAEADDRNOTAVAIL,19);MAP(EADDRINUSE,WSAEADDRINUSE,20);
    MAP(EAFNOSUPPORT,WSAEAFNOSUPPORT,21);MAP(EINPROGRESS,WSAEINPROGRESS,22);
    MAP(ENOTSOCK,WSAENOTSOCK,24);MAP(EFAULT,WSAEFAULT,29);MAP(ENETUNREACH,WSAENETUNREACH,30);
    MAP(EPROTONOSUPPORT,WSAEPROTONOSUPPORT,31);MAP(EPROTOTYPE,WSAEPROTOTYPE,32);MAP(EMFILE,WSAEMFILE,51);
#if !defined(_WIN32)
    if(value==EAGAIN)return 6;
    if(value==EPIPE)return 13;
    if(value==ENOMEM)return 18;
    if(value==EBADF)return 49;
#endif
#undef MAP
    return 41; // generic nsysnet error, never leak a Winsock/host errno number
}
constexpr uint32_t kMsgDontWait=0x20;
inline bool message_flags(uint32_t guest,int& host) {
    if(guest&~uint32_t(1|2|4|kMsgDontWait))return false;
    host=0;
    if(guest&1)host|=MSG_OOB;
    if(guest&2)host|=MSG_PEEK;
    if(guest&4)host|=MSG_DONTROUTE;
#if !defined(_WIN32) && defined(MSG_NOSIGNAL)
    host|=MSG_NOSIGNAL;
#endif
    return true;
}
}

