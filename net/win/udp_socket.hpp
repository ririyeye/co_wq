// udp_socket.hpp - UDP socket coroutine primitives
#pragma once

#include "iocp_reactor.hpp"
#include "io_waiter.hpp"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <stdexcept>
#include "worker.hpp"
#include <basetsd.h>
#ifndef _SSIZE_T_DEFINED
using ssize_t = SSIZE_T;
#define _SSIZE_T_DEFINED
#endif


namespace co_wq::net {

template <lockable lock, template <class> class Reactor> class fd_workqueue; // fwd

template <lockable lock, template <class> class Reactor = epoll_reactor> class udp_socket {
public:
    udp_socket() = delete; udp_socket(const udp_socket&) = delete; udp_socket& operator=(const udp_socket&) = delete;
    udp_socket(udp_socket&& o) noexcept : _exec(o._exec), _reactor(o._reactor), _sock(o._sock) { o._sock = INVALID_SOCKET; }
    udp_socket& operator=(udp_socket&& o) noexcept { if (this!=&o){ close(); _exec=o._exec; _reactor=o._reactor; _sock=o._sock; o._sock=INVALID_SOCKET;} return *this; }
    ~udp_socket(){ close(); }
    void close(){ if (_sock!=INVALID_SOCKET){ if (_reactor) _reactor->remove_fd((int)_sock); ::closesocket(_sock); _sock=INVALID_SOCKET; }}
    int native_handle() const { return (int)_sock; }
    workqueue<lock>& exec(){ return _exec; }
    Reactor<lock>* reactor(){ return _reactor; }
    struct recvfrom_awaiter : io_waiter_base {
        udp_socket& us; void* buf; size_t len; sockaddr_in* out_addr; socklen_t* out_len; ssize_t nrd{-1}; iocp_ovl ovl; WSABUF wbuf; DWORD flags{0};
        recvfrom_awaiter(udp_socket& s, void* b, size_t l, sockaddr_in* oa, socklen_t* ol):us(s),buf(b),len(l),out_addr(oa),out_len(ol){ ZeroMemory(&ovl,sizeof(ovl)); ovl.waiter=this; wbuf.len=(ULONG)l; wbuf.buf=(CHAR*)b; }
        bool await_ready() const noexcept { return false; }
        void await_suspend(std::coroutine_handle<> h){ this->h=h; this->func=&io_waiter_base::resume_cb; INIT_LIST_HEAD(&this->ws_node); int fromlen= sizeof(sockaddr_in); DWORD recvd=0; int r= WSARecvFrom(us._sock,&wbuf,1,&recvd,&flags,(sockaddr*)out_addr,(LPINT)&fromlen,&ovl,nullptr); if (r==0){ nrd=(ssize_t)recvd; us._reactor->post_completion(this); return; } int err=WSAGetLastError(); if (err!=WSA_IO_PENDING){ nrd=-1; us._reactor->post_completion(this);} if(out_len) *out_len=(socklen_t)fromlen; }
        ssize_t await_resume() noexcept { if(nrd==-1){ DWORD tr=0; DWORD fl=0; if (WSAGetOverlappedResult(us._sock,&ovl,&tr,FALSE,&fl)) nrd=(ssize_t)tr; } return nrd; }
    };
    recvfrom_awaiter recv_from(void* buf, size_t len, sockaddr_in* addr, socklen_t* addrlen){ return recvfrom_awaiter(*this,buf,len,addr,addrlen); }
    struct sendto_awaiter : io_waiter_base {
        udp_socket& us; const void* buf; size_t len; const sockaddr_in* dest; ssize_t nsent{-1}; iocp_ovl ovl; WSABUF wbuf;
        sendto_awaiter(udp_socket& s,const void* b,size_t l,const sockaddr_in* d):us(s),buf(b),len(l),dest(d){ ZeroMemory(&ovl,sizeof(ovl)); ovl.waiter=this; wbuf.len=(ULONG)l; wbuf.buf=(CHAR*)const_cast<void*>(b);} bool await_ready() const noexcept { return false; }
        void await_suspend(std::coroutine_handle<> h){ this->h=h; this->func=&io_waiter_base::resume_cb; INIT_LIST_HEAD(&this->ws_node); DWORD sent=0; int dlen=sizeof(sockaddr_in); int r= WSASendTo(us._sock,&wbuf,1,&sent,0,(const sockaddr*)dest,dlen,&ovl,nullptr); if(r==0){ nsent=(ssize_t)sent; us._reactor->post_completion(this); return;} int err=WSAGetLastError(); if(err!=WSA_IO_PENDING){ nsent=-1; us._reactor->post_completion(this);} }
        ssize_t await_resume() noexcept { if(nsent==-1){ DWORD tr=0; DWORD fl=0; if(WSAGetOverlappedResult(us._sock,&ovl,&tr,FALSE,&fl)) nsent=(ssize_t)tr; } return nsent; }
    };
    sendto_awaiter send_to(const void* buf, size_t len, const sockaddr_in& dest){ return sendto_awaiter(*this,buf,len,&dest); }
private:
    friend class fd_workqueue<lock, Reactor>;
    udp_socket(workqueue<lock>& e, Reactor<lock>& r):_exec(e),_reactor(&r){ _sock=::socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP); if(_sock==INVALID_SOCKET) throw std::runtime_error("udp socket failed"); u_long m=1; ioctlsocket(_sock,FIONBIO,&m); _reactor->add_fd((int)_sock);} 
    workqueue<lock>& _exec; Reactor<lock>* _reactor{nullptr}; SOCKET _sock{INVALID_SOCKET};
};

template <lockable lock, template <class> class Reactor>
inline Task<ssize_t, Work_Promise<lock, ssize_t>>
async_udp_recv_from(udp_socket<lock, Reactor>& s, void* buf, size_t len, sockaddr_in* addr, socklen_t* alen)
{ co_return co_await s.recv_from(buf,len,addr,alen); }
template <lockable lock, template <class> class Reactor>
inline Task<ssize_t, Work_Promise<lock, ssize_t>>
async_udp_send_to(udp_socket<lock, Reactor>& s, const void* buf, size_t len, const sockaddr_in& dest)
{ co_return co_await s.send_to(buf,len,dest); }

} // namespace co_wq::net

