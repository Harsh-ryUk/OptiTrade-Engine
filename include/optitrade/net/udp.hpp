#pragma once

// Thin RAII wrapper over an IPv4 UDP socket, restricted to POSIX calls that behave the same on
// Linux and macOS (no recvmmsg, no MSG_NOSIGNAL: UDP never raises SIGPIPE, so nothing needs
// suppressing). Move-only, no exceptions, no allocation. Failures are reported through return
// values and errno is left as the failing system call set it.
//
// Datagram size limits come from the host, not from this class: macOS refuses datagrams above
// net.inet.udp.maxdgram (9216 bytes by default), Linux allows up to 65507.

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <utility>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <unistd.h>

namespace optitrade::net {

namespace detail {

// inet_pton is strict dotted-quad (unlike inet_aton, it rejects "127.1" and trailing junk),
// which is what we want for addresses that come from configuration.
inline bool make_addr(const char* ip, std::uint16_t port, sockaddr_in& out) noexcept {
    if (ip == nullptr) return false;
    std::memset(&out, 0, sizeof out);
    out.sin_family = AF_INET;
    out.sin_port = htons(port);
    return inet_pton(AF_INET, ip, &out.sin_addr) == 1;
}

inline bool is_multicast(const in_addr& a) noexcept {
    return (ntohl(a.s_addr) & 0xF0000000u) == 0xE0000000u;  // 224.0.0.0/4
}

inline bool is_blank(const char* s) noexcept { return s == nullptr || *s == '\0'; }

// poll(2) counts in milliseconds. Round a positive microsecond timeout up so that a caller
// asking for 100 us waits 1 ms rather than degenerating into a busy return; negative means
// wait forever. (timeout_us == 0 never reaches poll: it is handled as a plain non-blocking read.)
constexpr int poll_timeout_ms(int timeout_us) noexcept {
    return timeout_us < 0 ? -1 : timeout_us / 1000 + (timeout_us % 1000 != 0 ? 1 : 0);
}

}  // namespace detail

class UdpSocket {
public:
    // An invalid socket (valid() == false); the state of a moved-from object.
    UdpSocket() noexcept = default;
    UdpSocket(const UdpSocket&) = delete;
    UdpSocket& operator=(const UdpSocket&) = delete;
    UdpSocket(UdpSocket&& o) noexcept
        : fd_(std::exchange(o.fd_, -1)), nonblocking_(std::exchange(o.nonblocking_, false)) {}
    UdpSocket& operator=(UdpSocket&& o) noexcept {
        if (this != &o) {
            close();
            fd_ = std::exchange(o.fd_, -1);
            nonblocking_ = std::exchange(o.nonblocking_, false);
        }
        return *this;
    }
    ~UdpSocket() { close(); }

    // Bound receiving socket. `bind_ip` null or empty binds all interfaces; `port` 0 lets the
    // kernel pick one (read it back with local_port()). `rcvbuf_bytes` > 0 requests a larger
    // kernel receive buffer; the kernel may clamp it, so this is best effort and never a
    // failure. Returns an invalid socket if the address is malformed or the bind fails.
    static UdpSocket receiver(const char* bind_ip, std::uint16_t port, int rcvbuf_bytes = 0) noexcept {
        sockaddr_in addr{};
        if (detail::is_blank(bind_ip)) {
            addr.sin_family = AF_INET;
            addr.sin_port = htons(port);
            addr.sin_addr.s_addr = htonl(INADDR_ANY);
        } else if (!detail::make_addr(bind_ip, port, addr)) {
            return UdpSocket{};
        }
        UdpSocket s{::socket(AF_INET, SOCK_DGRAM, 0)};
        if (!s.valid()) return UdpSocket{};

        // Several feed handlers on one host may subscribe to the same multicast group and
        // port, which needs address (and on BSD-derived systems port) reuse. Unicast
        // receivers keep exclusive ownership so a stale process cannot share a port silently.
        if (detail::is_multicast(addr.sin_addr)) {
            const int on = 1;
            (void)::setsockopt(s.fd_, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
#ifdef SO_REUSEPORT
            (void)::setsockopt(s.fd_, SOL_SOCKET, SO_REUSEPORT, &on, sizeof on);
#endif
        }
        if (rcvbuf_bytes > 0) {
            (void)::setsockopt(s.fd_, SOL_SOCKET, SO_RCVBUF, &rcvbuf_bytes, sizeof rcvbuf_bytes);
        }
        if (::bind(s.fd_, reinterpret_cast<const sockaddr*>(&addr), sizeof addr) != 0) {
            return UdpSocket{};
        }
        return s;
    }

    // Unbound socket for sending; the kernel assigns a source port on first use.
    static UdpSocket sender() noexcept { return UdpSocket{::socket(AF_INET, SOCK_DGRAM, 0)}; }

    bool valid() const noexcept { return fd_ >= 0; }
    // Underlying descriptor for poll/epoll integration; -1 when invalid. Ownership stays here.
    int native_handle() const noexcept { return fd_; }

    void close() noexcept {
        if (fd_ >= 0) (void)::close(fd_);
        fd_ = -1;
        nonblocking_ = false;
    }

    // Local port in host byte order, or 0 for an invalid socket.
    std::uint16_t local_port() const noexcept {
        if (fd_ < 0) return 0;
        sockaddr_in a{};
        socklen_t len = sizeof a;
        if (::getsockname(fd_, reinterpret_cast<sockaddr*>(&a), &len) != 0 || a.sin_family != AF_INET) {
            return 0;
        }
        return ntohs(a.sin_port);
    }

    // Subscribes to an IPv4 multicast group on the interface owning `interface_ip` (null or
    // empty: the kernel's default route choice). False for a malformed or non-multicast
    // group, a malformed interface, or if the host refuses the membership (many containers
    // and sandboxes have no multicast support, so callers must handle failure).
    bool join_multicast(const char* group_ip, const char* interface_ip) noexcept {
        if (fd_ < 0) return false;
        sockaddr_in group{};
        if (!detail::make_addr(group_ip, 0, group) || !detail::is_multicast(group.sin_addr)) return false;
        ip_mreq mreq{};
        mreq.imr_multiaddr = group.sin_addr;
        if (detail::is_blank(interface_ip)) {
            mreq.imr_interface.s_addr = htonl(INADDR_ANY);
        } else {
            sockaddr_in iface{};
            if (!detail::make_addr(interface_ip, 0, iface)) return false;
            mreq.imr_interface = iface.sin_addr;
        }
        return ::setsockopt(fd_, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof mreq) == 0;
    }

    // In nonblocking mode recv() never waits (its timeout is ignored), which lets a busy-poll
    // receiver keep the same call for both styles. Also sets O_NONBLOCK on the descriptor so
    // code using native_handle() directly sees the same behaviour.
    bool set_nonblocking(bool on) noexcept {
        if (fd_ < 0) return false;
        const int flags = ::fcntl(fd_, F_GETFL, 0);
        if (flags < 0) return false;
        const int wanted = on ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
        if (wanted != flags && ::fcntl(fd_, F_SETFL, wanted) != 0) return false;
        nonblocking_ = on;
        return true;
    }
    bool nonblocking() const noexcept { return nonblocking_; }

    // Receives one datagram into `buf`.
    //   > 0  bytes received (datagram boundaries are preserved)
    //     0  nothing arrived within the timeout (also returned for an empty datagram and
    //        after a signal interrupted the wait, so a loop can re-check its stop flag)
    //    -1  error, errno set. EMSGSIZE means the datagram was larger than `buf`: it has been
    //        consumed and discarded rather than delivered truncated, because a cut-off
    //        packet is worse than a lost one.
    // timeout_us: 0 = poll once, > 0 = wait up to that long, < 0 = wait indefinitely. The wait
    // uses poll(2), whose resolution is 1 ms, so non-zero timeouts are rounded up to whole
    // milliseconds; spin on 0 when microseconds matter.
    std::ptrdiff_t recv(std::span<std::byte> buf, int timeout_us) noexcept {
        if (fd_ < 0) {
            errno = EBADF;
            return -1;
        }
        if (!nonblocking_ && timeout_us != 0) {
            pollfd p{};
            p.fd = fd_;
            p.events = POLLIN;
            const int rc = ::poll(&p, 1, detail::poll_timeout_ms(timeout_us));
            if (rc == 0) return 0;
            if (rc < 0) return errno == EINTR ? 0 : -1;
            // POLLERR / POLLNVAL fall through: recvmsg below reports the underlying error.
        }
        iovec iov{};
        iov.iov_base = buf.data();
        iov.iov_len = buf.size();
        msghdr msg{};
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        ssize_t n;
        do {
            n = ::recvmsg(fd_, &msg, MSG_DONTWAIT);
        } while (n < 0 && errno == EINTR);
        if (n < 0) return (errno == EAGAIN || errno == EWOULDBLOCK) ? 0 : -1;
        if ((msg.msg_flags & MSG_TRUNC) != 0) {
            errno = EMSGSIZE;
            return -1;
        }
        return static_cast<std::ptrdiff_t>(n);
    }

    // Sends one datagram to a dotted-quad IPv4 address. True only if the whole datagram was
    // handed to the kernel (UDP sends are all or nothing). An empty payload is refused because
    // recv() reports an empty datagram as "nothing received". A full send buffer (ENOBUFS,
    // EAGAIN) is reported as false rather than retried; the caller decides whether to drop.
    bool send_to(const char* ip, std::uint16_t port, std::span<const std::byte> data) noexcept {
        sockaddr_in to{};
        if (fd_ < 0 || data.empty() || !detail::make_addr(ip, port, to)) return false;
        ssize_t n;
        do {
            n = ::sendto(fd_, data.data(), data.size(), 0, reinterpret_cast<const sockaddr*>(&to), sizeof to);
        } while (n < 0 && errno == EINTR);
        return n >= 0 && static_cast<std::size_t>(n) == data.size();
    }

private:
    explicit UdpSocket(int fd) noexcept : fd_(fd) {}

    int fd_{-1};
    bool nonblocking_{false};
};

}  // namespace optitrade::net
