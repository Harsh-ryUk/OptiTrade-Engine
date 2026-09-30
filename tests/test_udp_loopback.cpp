// UdpSocket over the loopback interface. Every receiver binds an ephemeral port (no fixed
// ports, so parallel test runs cannot collide). The multicast round trip needs a host that
// lets a socket join a group on the loopback interface; sandboxes and containers often do not,
// so that one test probes the host first and prints a skip reason instead of failing.

#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <utility>
#include <vector>

#include <arpa/inet.h>
#include <csignal>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "check.hpp"
#include "optitrade/net/mold64.hpp"
#include "optitrade/net/udp.hpp"

namespace {

using optitrade::net::UdpSocket;
using Bytes = std::vector<std::byte>;
using Clock = std::chrono::steady_clock;

constexpr const char* kLoopback = "127.0.0.1";
constexpr int kWaitUs = 2'000'000;  // generous: only reached when a test is about to fail

Bytes pattern(std::size_t n, unsigned salt) {
    Bytes b(n);
    for (std::size_t i = 0; i < n; ++i) b[i] = static_cast<std::byte>((i * 31 + salt) & 0xFF);
    return b;
}
std::span<const std::byte> view(const Bytes& b) { return {b.data(), b.size()}; }

long elapsed_ms(Clock::time_point since) {
    return static_cast<long>(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - since).count());
}

bool fd_is_open(int fd) { return ::fcntl(fd, F_GETFD) != -1; }

void skip(const char* test, const char* why) { std::printf("[skip] %s: %s\n", test, why); }

// Asks the host directly (plain syscalls, not UdpSocket) whether joining `group` on `iface` is
// possible, so a bug in join_multicast can never be mistaken for a host without multicast.
bool host_can_join(const char* group, const char* iface) {
    const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return false;
    ip_mreq m{};
    const bool parsed = inet_pton(AF_INET, group, &m.imr_multiaddr) == 1 && inet_pton(AF_INET, iface, &m.imr_interface) == 1;
    const bool ok = parsed && ::setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &m, sizeof m) == 0;
    (void)::close(fd);
    return ok;
}

template <class T>
T get_ip_opt(const UdpSocket& s, int name) {
    T v{};
    socklen_t len = sizeof v;
    if (::getsockopt(s.native_handle(), IPPROTO_IP, name, &v, &len) != 0) return T{};
    return v;
}

bool cloexec(const UdpSocket& s) {
    const int flags = ::fcntl(s.native_handle(), F_GETFD);
    return flags >= 0 && (flags & FD_CLOEXEC) != 0;
}

}  // namespace

OT_TEST(receiver_binds_an_ephemeral_port) {
    UdpSocket a = UdpSocket::receiver(kLoopback, 0);
    UdpSocket b = UdpSocket::receiver(kLoopback, 0);
    OT_CHECK(a.valid());
    OT_CHECK(b.valid());
    OT_CHECK(a.local_port() != 0);
    OT_CHECK(b.local_port() != 0);
    OT_CHECK(a.local_port() != b.local_port());
    OT_CHECK(a.native_handle() >= 0);
}

OT_TEST(send_and_receive_one_datagram) {
    UdpSocket rx = UdpSocket::receiver(kLoopback, 0);
    UdpSocket tx = UdpSocket::sender();
    OT_CHECK(rx.valid() && tx.valid());
    const Bytes msg = pattern(37, 5);
    OT_CHECK(tx.send_to(kLoopback, rx.local_port(), view(msg)));
    Bytes buf(2048, std::byte{0xEE});
    const std::ptrdiff_t n = rx.recv(std::span<std::byte>(buf), kWaitUs);
    OT_CHECK_EQ(n, std::ptrdiff_t{37});
    OT_CHECK(n == 37 && std::memcmp(buf.data(), msg.data(), 37) == 0);
    OT_CHECK(buf[37] == std::byte{0xEE});  // nothing written past the datagram
}

OT_TEST(datagram_boundaries_and_order_are_preserved) {
    UdpSocket rx = UdpSocket::receiver(kLoopback, 0);
    UdpSocket tx = UdpSocket::sender();
    const std::size_t sizes[] = {1, 100, 1472, 2, 999};
    std::vector<Bytes> sent;
    for (std::size_t i = 0; i < 5; ++i) {
        sent.push_back(pattern(sizes[i], static_cast<unsigned>(i)));
        OT_CHECK(tx.send_to(kLoopback, rx.local_port(), view(sent.back())));
    }
    Bytes buf(4096);
    for (std::size_t i = 0; i < 5; ++i) {
        const std::ptrdiff_t n = rx.recv(std::span<std::byte>(buf), kWaitUs);
        OT_CHECK_EQ(n, static_cast<std::ptrdiff_t>(sizes[i]));
        OT_CHECK(n == static_cast<std::ptrdiff_t>(sizes[i]) && std::memcmp(buf.data(), sent[i].data(), sizes[i]) == 0);
    }
}

OT_TEST(burst_of_small_datagrams_arrives_in_order) {
    UdpSocket rx = UdpSocket::receiver(kLoopback, 0, 1 << 20);
    UdpSocket tx = UdpSocket::sender();
    for (std::uint8_t i = 0; i < 100; ++i) {
        const std::byte b[2] = {std::byte{i}, std::byte{0x5A}};
        OT_CHECK(tx.send_to(kLoopback, rx.local_port(), std::span<const std::byte>(b, 2)));
    }
    std::byte buf[16];
    for (std::uint8_t i = 0; i < 100; ++i) {
        const std::ptrdiff_t n = rx.recv(std::span<std::byte>(buf), kWaitUs);
        OT_CHECK_EQ(n, std::ptrdiff_t{2});
        if (n != 2) return;
        OT_CHECK(buf[0] == std::byte{i});
    }
}

OT_TEST(timeout_returns_zero) {
    UdpSocket rx = UdpSocket::receiver(kLoopback, 0);
    Bytes buf(64);
    auto t0 = Clock::now();
    OT_CHECK_EQ(rx.recv(std::span<std::byte>(buf), 20'000), std::ptrdiff_t{0});
    // poll may return a little early on some kernels; half the timeout is an unambiguous
    // sign that the call waited at all, while no upper bound keeps it robust on loaded CI.
    OT_CHECK(elapsed_ms(t0) >= 10);
    // A sub-millisecond timeout still terminates (it is rounded up, see the next test).
    OT_CHECK_EQ(rx.recv(std::span<std::byte>(buf), 1), std::ptrdiff_t{0});
    // Zero means "look once": immediate.
    t0 = Clock::now();
    OT_CHECK_EQ(rx.recv(std::span<std::byte>(buf), 0), std::ptrdiff_t{0});
    OT_CHECK(elapsed_ms(t0) < 1000);
}

OT_TEST(poll_timeout_rounds_up_to_whole_milliseconds) {
    using optitrade::net::detail::poll_timeout_ms;
    OT_CHECK_EQ(poll_timeout_ms(-1), -1);
    OT_CHECK_EQ(poll_timeout_ms(-2'000'000), -1);
    OT_CHECK_EQ(poll_timeout_ms(1), 1);
    OT_CHECK_EQ(poll_timeout_ms(999), 1);
    OT_CHECK_EQ(poll_timeout_ms(1000), 1);
    OT_CHECK_EQ(poll_timeout_ms(1001), 2);
    OT_CHECK_EQ(poll_timeout_ms(20'000), 20);
    OT_CHECK_EQ(poll_timeout_ms(2'147'483'647), 2'147'484);  // INT_MAX microseconds does not overflow
}

OT_TEST(negative_timeout_returns_pending_data_immediately) {
    UdpSocket rx = UdpSocket::receiver(kLoopback, 0);
    UdpSocket tx = UdpSocket::sender();
    const Bytes msg = pattern(9, 1);
    // recv(-1) waits forever, so never call it when the send failed and nothing can arrive.
    if (!tx.send_to(kLoopback, rx.local_port(), view(msg))) {
        OT_CHECK(false);
        return;
    }
    Bytes buf(64);
    OT_CHECK_EQ(rx.recv(std::span<std::byte>(buf), -1), std::ptrdiff_t{9});
}

OT_TEST(large_datagram) {
    // 8192 bytes: far above any MTU (exercises IP fragmentation on real links, 16 KiB MTU on
    // loopback) yet below macOS's default net.inet.udp.maxdgram of 9216.
    UdpSocket rx = UdpSocket::receiver(kLoopback, 0, 1 << 20);
    UdpSocket tx = UdpSocket::sender();
    const Bytes msg = pattern(8192, 77);
    OT_CHECK(tx.send_to(kLoopback, rx.local_port(), view(msg)));
    Bytes buf(16384);
    const std::ptrdiff_t n = rx.recv(std::span<std::byte>(buf), kWaitUs);
    OT_CHECK_EQ(n, std::ptrdiff_t{8192});
    OT_CHECK(n == 8192 && std::memcmp(buf.data(), msg.data(), 8192) == 0);
}

OT_TEST(datagram_beyond_the_ip_limit_is_refused) {
    UdpSocket rx = UdpSocket::receiver(kLoopback, 0);
    UdpSocket tx = UdpSocket::sender();
    const Bytes msg = pattern(70'000, 3);  // > 65507 bytes: cannot be a single UDP datagram
    OT_CHECK(!tx.send_to(kLoopback, rx.local_port(), view(msg)));
    Bytes buf(128);
    OT_CHECK_EQ(rx.recv(std::span<std::byte>(buf), 5'000), std::ptrdiff_t{0});
}

OT_TEST(short_buffer_reports_error_and_discards_the_datagram) {
    UdpSocket rx = UdpSocket::receiver(kLoopback, 0);
    UdpSocket tx = UdpSocket::sender();
    const Bytes big = pattern(100, 1);
    const Bytes next = pattern(10, 2);
    OT_CHECK(tx.send_to(kLoopback, rx.local_port(), view(big)));
    OT_CHECK(tx.send_to(kLoopback, rx.local_port(), view(next)));
    Bytes small(50);
    errno = 0;
    OT_CHECK_EQ(rx.recv(std::span<std::byte>(small), kWaitUs), std::ptrdiff_t{-1});
    OT_CHECK_EQ(errno, EMSGSIZE);
    // The cut-off datagram is gone; the following one is delivered intact.
    Bytes buf(50);
    OT_CHECK_EQ(rx.recv(std::span<std::byte>(buf), kWaitUs), std::ptrdiff_t{10});
    OT_CHECK(std::memcmp(buf.data(), next.data(), 10) == 0);
    // A datagram that exactly fills the buffer is not "truncated".
    OT_CHECK(tx.send_to(kLoopback, rx.local_port(), view(pattern(50, 9))));
    OT_CHECK_EQ(rx.recv(std::span<std::byte>(buf), kWaitUs), std::ptrdiff_t{50});
}

extern "C" void ot_test_ignore_signal(int) {}

OT_TEST(interrupted_wait_returns_zero_so_callers_can_recheck_their_stop_flag) {
    UdpSocket rx = UdpSocket::receiver(kLoopback, 0);
    // A handler installed without SA_RESTART makes poll(2) fail with EINTR when it fires.
    struct sigaction sa {};
    sa.sa_handler = ot_test_ignore_signal;
    sigemptyset(&sa.sa_mask);
    struct sigaction old {};
    OT_CHECK(::sigaction(SIGALRM, &sa, &old) == 0);
    itimerval timer{};
    timer.it_value.tv_usec = 30'000;
    OT_CHECK(::setitimer(ITIMER_REAL, &timer, nullptr) == 0);

    Bytes buf(16);
    const auto t0 = Clock::now();
    errno = 0;
    const std::ptrdiff_t n = rx.recv(std::span<std::byte>(buf), 5'000'000);
    const long waited = elapsed_ms(t0);

    itimerval off{};
    (void)::setitimer(ITIMER_REAL, &off, nullptr);
    (void)::sigaction(SIGALRM, &old, nullptr);
    OT_CHECK_EQ(n, std::ptrdiff_t{0});  // not an error
    OT_CHECK(waited < 4000);            // returned because of the signal, not the 5 s timeout
}

OT_TEST(nonblocking_mode_never_waits) {
    UdpSocket rx = UdpSocket::receiver(kLoopback, 0);
    UdpSocket tx = UdpSocket::sender();
    Bytes buf(64);
    OT_CHECK(!rx.nonblocking());
    OT_CHECK(rx.set_nonblocking(true));
    OT_CHECK(rx.nonblocking());
    OT_CHECK((::fcntl(rx.native_handle(), F_GETFL) & O_NONBLOCK) != 0);

    // The timeout is ignored: a 5 s request must come back at once.
    const auto t0 = Clock::now();
    OT_CHECK_EQ(rx.recv(std::span<std::byte>(buf), 5'000'000), std::ptrdiff_t{0});
    OT_CHECK(elapsed_ms(t0) < 1000);

    // Data that arrives is picked up by polling (loopback delivery is not instantaneous on
    // every kernel, so spin with a deadline rather than assuming the first call sees it).
    const Bytes msg = pattern(12, 4);
    OT_CHECK(tx.send_to(kLoopback, rx.local_port(), view(msg)));
    std::ptrdiff_t n = 0;
    const auto deadline = Clock::now() + std::chrono::seconds(2);
    while (n == 0 && Clock::now() < deadline) n = rx.recv(std::span<std::byte>(buf), 0);
    OT_CHECK_EQ(n, std::ptrdiff_t{12});

    // Back to blocking: the timeout is honoured again.
    OT_CHECK(rx.set_nonblocking(false));
    OT_CHECK(!rx.nonblocking());
    OT_CHECK((::fcntl(rx.native_handle(), F_GETFL) & O_NONBLOCK) == 0);
    const auto t1 = Clock::now();
    OT_CHECK_EQ(rx.recv(std::span<std::byte>(buf), 20'000), std::ptrdiff_t{0});
    OT_CHECK(elapsed_ms(t1) >= 10);
}

OT_TEST(invalid_addresses_are_rejected) {
    UdpSocket tx = UdpSocket::sender();
    UdpSocket rx = UdpSocket::receiver(kLoopback, 0);
    const Bytes msg = pattern(4, 0);
    const std::uint16_t port = rx.local_port();
    OT_CHECK(!tx.send_to("not.an.ip", port, view(msg)));
    OT_CHECK(!tx.send_to("256.0.0.1", port, view(msg)));
    OT_CHECK(!tx.send_to("127.0.0", port, view(msg)));
    OT_CHECK(!tx.send_to("127.0.0.1 ", port, view(msg)));  // trailing junk
    OT_CHECK(!tx.send_to("localhost", port, view(msg)));   // no name resolution by design
    OT_CHECK(!tx.send_to("", port, view(msg)));
    OT_CHECK(!tx.send_to(nullptr, port, view(msg)));
    Bytes buf(16);
    OT_CHECK_EQ(rx.recv(std::span<std::byte>(buf), 5'000), std::ptrdiff_t{0});  // nothing leaked out

    OT_CHECK(!UdpSocket::receiver("not.an.ip", 0).valid());
    OT_CHECK(!UdpSocket::receiver("256.1.1.1", 0).valid());
    OT_CHECK(!UdpSocket::receiver("127.0.0.1x", 0).valid());
}

OT_TEST(empty_payload_is_refused) {
    // recv() reports an empty datagram as "no data", so the sender never produces one.
    UdpSocket tx = UdpSocket::sender();
    UdpSocket rx = UdpSocket::receiver(kLoopback, 0);
    OT_CHECK(!tx.send_to(kLoopback, rx.local_port(), std::span<const std::byte>{}));
}

OT_TEST(blank_bind_address_listens_on_all_interfaces) {
    for (const char* any : {static_cast<const char*>(nullptr), ""}) {
        UdpSocket rx = UdpSocket::receiver(any, 0);
        UdpSocket tx = UdpSocket::sender();
        OT_CHECK(rx.valid());
        const Bytes msg = pattern(5, 8);
        OT_CHECK(tx.send_to(kLoopback, rx.local_port(), view(msg)));
        Bytes buf(16);
        OT_CHECK_EQ(rx.recv(std::span<std::byte>(buf), kWaitUs), std::ptrdiff_t{5});
    }
}

OT_TEST(a_port_in_use_cannot_be_bound_twice) {
    UdpSocket first = UdpSocket::receiver(kLoopback, 0);
    OT_CHECK(first.valid());
    UdpSocket second = UdpSocket::receiver(kLoopback, first.local_port());
    OT_CHECK(!second.valid());
    OT_CHECK(first.valid());  // the failed attempt must not disturb the owner
}

OT_TEST(moved_from_socket_is_invalid) {
    UdpSocket a = UdpSocket::receiver(kLoopback, 0);
    const std::uint16_t port = a.local_port();
    const int fd = a.native_handle();
    OT_CHECK(port != 0);

    UdpSocket b = std::move(a);
    OT_CHECK(!a.valid());  // NOLINT: moved-from state is specified
    OT_CHECK_EQ(a.native_handle(), -1);
    OT_CHECK_EQ(a.local_port(), std::uint16_t{0});
    OT_CHECK(!a.set_nonblocking(true));
    OT_CHECK(!a.join_multicast("239.1.2.3", nullptr));
    Bytes buf(8);
    errno = 0;
    OT_CHECK_EQ(a.recv(std::span<std::byte>(buf), 0), std::ptrdiff_t{-1});
    OT_CHECK_EQ(errno, EBADF);
    OT_CHECK(!a.send_to(kLoopback, port, view(pattern(3, 0))));
    a.close();  // closing an invalid socket is a no-op

    OT_CHECK(b.valid());
    OT_CHECK_EQ(b.native_handle(), fd);
    OT_CHECK_EQ(b.local_port(), port);

    // Move assignment closes the socket it overwrites and takes over the other one.
    UdpSocket c = UdpSocket::receiver(kLoopback, 0);
    const int old_fd = c.native_handle();
    c = std::move(b);
    OT_CHECK(!b.valid());  // NOLINT: moved-from state is specified
    OT_CHECK(c.valid());
    OT_CHECK_EQ(c.local_port(), port);
    OT_CHECK(!fd_is_open(old_fd));
}

OT_TEST(nonblocking_flag_travels_with_the_socket) {
    UdpSocket a = UdpSocket::receiver(kLoopback, 0);
    OT_CHECK(a.set_nonblocking(true));
    UdpSocket b = std::move(a);
    OT_CHECK(b.nonblocking());
    OT_CHECK(!a.nonblocking());  // NOLINT: moved-from state is specified
}

OT_TEST(destructor_and_close_release_the_descriptor) {
    int fd = -1;
    {
        UdpSocket s = UdpSocket::sender();
        fd = s.native_handle();
        OT_CHECK(fd >= 0 && fd_is_open(fd));
    }
    OT_CHECK(!fd_is_open(fd));

    UdpSocket t = UdpSocket::receiver(kLoopback, 0);
    fd = t.native_handle();
    t.close();
    OT_CHECK(!t.valid());
    OT_CHECK(!fd_is_open(fd));
    t.close();  // idempotent
}

OT_TEST(multicast_join_rejects_bad_arguments) {
    UdpSocket rx = UdpSocket::receiver(nullptr, 0);
    OT_CHECK(rx.valid());
    OT_CHECK(!rx.join_multicast("not-an-ip", nullptr));
    OT_CHECK(!rx.join_multicast("999.1.1.1", nullptr));
    OT_CHECK(!rx.join_multicast("", nullptr));
    OT_CHECK(!rx.join_multicast(nullptr, nullptr));
    // Well-formed but unicast: refuse it up front instead of relying on the host's error.
    OT_CHECK(!rx.join_multicast("127.0.0.1", nullptr));
    OT_CHECK(!rx.join_multicast("223.255.255.255", nullptr));  // just below 224.0.0.0
    OT_CHECK(!rx.join_multicast("240.0.0.1", nullptr));         // just above 239.255.255.255
    OT_CHECK(!rx.join_multicast("239.1.2.3", "not-an-ip"));     // bad interface address

    OT_CHECK(rx.valid());
}

OT_TEST(multicast_join_and_round_trip_on_loopback) {
    const char* group = "239.255.42.99";
    // The upper end of the group range: a mask narrower than 224.0.0.0/4 would refuse it up front.
    const char* edge = "239.255.255.250";
    if (!host_can_join(group, kLoopback) || !host_can_join(edge, kLoopback)) {
        skip("multicast_join_and_round_trip_on_loopback", "host refuses IP_ADD_MEMBERSHIP on 127.0.0.1");
        return;
    }
    UdpSocket rx = UdpSocket::receiver(nullptr, 0);
    OT_CHECK(rx.valid());
    OT_CHECK(rx.join_multicast(group, kLoopback));
    OT_CHECK(rx.join_multicast(edge, kLoopback));

    UdpSocket tx = UdpSocket::sender();
    OT_CHECK(tx.set_multicast_interface(kLoopback));
    OT_CHECK(tx.set_multicast_loop(true));
    OT_CHECK(tx.set_multicast_ttl(1));
    const Bytes msg = pattern(37, 11);
    errno = 0;
    if (!tx.send_to(group, rx.local_port(), view(msg))) {
        const int e = errno;
        if (e == ENETUNREACH || e == EHOSTUNREACH || e == ENODEV || e == EADDRNOTAVAIL || e == EPERM || e == EACCES) {
            skip("multicast_join_and_round_trip_on_loopback", "host cannot route multicast out of 127.0.0.1");
            return;
        }
        OT_CHECK(false);
        return;
    }
    Bytes buf(2048, std::byte{0xEE});
    const std::ptrdiff_t n = rx.recv(std::span<std::byte>(buf), kWaitUs);
    OT_CHECK_EQ(n, std::ptrdiff_t{37});
    OT_CHECK(n == 37 && std::memcmp(buf.data(), msg.data(), 37) == 0);

    // With host-local delivery switched off the sender's own datagram must not come back.
    OT_CHECK(tx.set_multicast_loop(false));
    OT_CHECK(tx.send_to(group, rx.local_port(), view(msg)));
    OT_CHECK_EQ(rx.recv(std::span<std::byte>(buf), 200'000), std::ptrdiff_t{0});
    OT_CHECK(tx.set_multicast_loop(true));
    OT_CHECK(tx.send_to(group, rx.local_port(), view(msg)));
    OT_CHECK_EQ(rx.recv(std::span<std::byte>(buf), kWaitUs), std::ptrdiff_t{37});
}

OT_TEST(multicast_sender_options_are_applied) {
    UdpSocket tx = UdpSocket::sender();
    OT_CHECK(tx.valid());

    OT_CHECK(tx.set_multicast_interface(kLoopback));
    OT_CHECK_EQ(get_ip_opt<in_addr>(tx, IP_MULTICAST_IF).s_addr, htonl(INADDR_LOOPBACK));
    OT_CHECK(!tx.set_multicast_interface("not-an-ip"));  // refused and the setting is untouched
    OT_CHECK_EQ(get_ip_opt<in_addr>(tx, IP_MULTICAST_IF).s_addr, htonl(INADDR_LOOPBACK));
    OT_CHECK(tx.set_multicast_interface(nullptr));  // back to the kernel's choice
    OT_CHECK_EQ(get_ip_opt<in_addr>(tx, IP_MULTICAST_IF).s_addr, htonl(INADDR_ANY));
    OT_CHECK(tx.set_multicast_interface(kLoopback));
    OT_CHECK(tx.set_multicast_interface(""));
    OT_CHECK_EQ(get_ip_opt<in_addr>(tx, IP_MULTICAST_IF).s_addr, htonl(INADDR_ANY));

    // An address no local interface owns: the host's refusal must be reported, not swallowed.
    // (TEST-NET-1, never routable. Whether the host refuses is asked of the kernel directly.)
    in_addr foreign{};
    inet_pton(AF_INET, "192.0.2.123", &foreign);
    const bool host_refuses = ::setsockopt(UdpSocket::sender().native_handle(), IPPROTO_IP, IP_MULTICAST_IF, &foreign, sizeof foreign) != 0;
    if (host_refuses) {
        OT_CHECK(!tx.set_multicast_interface("192.0.2.123"));
    } else {
        skip("multicast_sender_options_are_applied", "host accepts a non-local IP_MULTICAST_IF address");
    }

    OT_CHECK(tx.set_multicast_ttl(7));
    OT_CHECK_EQ(int{get_ip_opt<unsigned char>(tx, IP_MULTICAST_TTL)}, 7);
    OT_CHECK(tx.set_multicast_ttl(255));
    OT_CHECK_EQ(int{get_ip_opt<unsigned char>(tx, IP_MULTICAST_TTL)}, 255);
    OT_CHECK(tx.set_multicast_ttl(0));
    OT_CHECK_EQ(int{get_ip_opt<unsigned char>(tx, IP_MULTICAST_TTL)}, 0);
    OT_CHECK(tx.set_multicast_ttl(9));
    OT_CHECK(!tx.set_multicast_ttl(-1));
    OT_CHECK(!tx.set_multicast_ttl(256));
    OT_CHECK_EQ(int{get_ip_opt<unsigned char>(tx, IP_MULTICAST_TTL)}, 9);  // rejected values change nothing

    OT_CHECK(tx.set_multicast_loop(false));
    OT_CHECK_EQ(int{get_ip_opt<unsigned char>(tx, IP_MULTICAST_LOOP)}, 0);
    OT_CHECK(tx.set_multicast_loop(true));
    OT_CHECK_EQ(int{get_ip_opt<unsigned char>(tx, IP_MULTICAST_LOOP)}, 1);

    UdpSocket none;
    OT_CHECK(!none.set_multicast_interface(kLoopback));
    OT_CHECK(!none.set_multicast_loop(true));
    OT_CHECK(!none.set_multicast_ttl(1));
}

OT_TEST(sockets_are_close_on_exec) {
    // Otherwise an exec'd helper would inherit the descriptor and keep the port bound.
    UdpSocket tx = UdpSocket::sender();
    UdpSocket rx = UdpSocket::receiver(kLoopback, 0);
    UdpSocket any = UdpSocket::receiver(nullptr, 0, 1 << 16);
    UdpSocket grp = UdpSocket::receiver("239.255.42.98", 0);
    OT_CHECK(tx.valid() && rx.valid() && any.valid());
    OT_CHECK(cloexec(tx));
    OT_CHECK(cloexec(rx));
    OT_CHECK(cloexec(any));
    if (grp.valid()) OT_CHECK(cloexec(grp));
    OT_CHECK(rx.set_nonblocking(true));  // changing file status flags must not clear it
    OT_CHECK(cloexec(rx));
    UdpSocket moved = std::move(rx);
    OT_CHECK(cloexec(moved));
}

OT_TEST(multicast_group_bind_address_is_accepted) {
    // Binding to a group address is how a multicast-only listener filters traffic; it must
    // not need an actual membership to bind. Skip silently where the host refuses.
    UdpSocket rx = UdpSocket::receiver("239.255.42.98", 0);
    if (rx.valid()) OT_CHECK(rx.local_port() != 0);
}

OT_TEST(moldudp64_packets_travel_through_the_socket) {
    using namespace optitrade::net::mold64;
    UdpSocket rx = UdpSocket::receiver(kLoopback, 0);
    UdpSocket tx = UdpSocket::sender();
    std::array<char, kSessionSize> session{};
    std::memcpy(session.data(), "LOOPBACK01", kSessionSize);

    Bytes storage(1472);
    SequenceTracker tracker;
    std::uint64_t next_seq = 1000;
    std::size_t delivered = 0;
    Bytes buf(2048);

    for (int round = 0; round < 3; ++round) {
        PacketBuilder b(std::span<std::byte>(storage), session, next_seq);
        for (int i = 0; i < 4; ++i) OT_CHECK(b.add(view(pattern(10 + static_cast<std::size_t>(i), static_cast<unsigned>(round)))));
        const auto pkt = b.finish();
        if (round != 1) {  // packet 1 is "lost": the tracker must see a gap of four
            OT_CHECK(tx.send_to(kLoopback, rx.local_port(), pkt));
        }
        next_seq += b.count();
    }
    // A heartbeat naming the next sequence closes the loop and would expose a lost tail.
    Bytes hb(kHeaderSize);
    OT_CHECK_EQ(write_heartbeat(std::span<std::byte>(hb), session, next_seq), kHeaderSize);
    OT_CHECK(tx.send_to(kLoopback, rx.local_port(), view(hb)));

    for (int i = 0; i < 3; ++i) {
        const std::ptrdiff_t n = rx.recv(std::span<std::byte>(buf), kWaitUs);
        OT_CHECK(n > 0);
        if (n <= 0) return;
        Header h;
        const auto st = for_each_message(std::span<const std::byte>(buf.data(), static_cast<std::size_t>(n)), h,
                                         [&](std::uint64_t seq, std::span<const std::byte> m) {
                                             OT_CHECK(m.size() >= 10 && m.size() <= 13);
                                             OT_CHECK(seq >= 1000);
                                             ++delivered;
                                         });
        OT_CHECK(st == optitrade::DecodeStatus::ok);
        const SequenceCheck c = tracker.on_packet(h);
        if (i == 0) OT_CHECK(c.result == SequenceCheck::Result::in_order);
        if (i == 1) {
            OT_CHECK(c.result == SequenceCheck::Result::gap);
            OT_CHECK_EQ(c.missing, std::uint64_t{4});
        }
        if (i == 2) OT_CHECK(c.result == SequenceCheck::Result::in_order);  // the heartbeat, in sync
    }
    OT_CHECK_EQ(delivered, std::size_t{8});
    OT_CHECK_EQ(tracker.gaps(), std::uint64_t{1});
    OT_CHECK_EQ(tracker.missing_messages(), std::uint64_t{4});
}

OT_TEST_MAIN()
