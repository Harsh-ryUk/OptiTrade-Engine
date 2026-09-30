// Capture file writer/reader, Nasdaq BinaryFILE reader and the in-memory source.
// Expected file contents are written out byte by byte from the format description
// in replay/capture.hpp rather than produced by the writer under test.

#include <dirent.h>
#include <fcntl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <csignal>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

#include "check.hpp"
#include "optitrade/core/rng.hpp"
#include "optitrade/itch/decoder.hpp"
#include "optitrade/itch/encoder.hpp"
#include "optitrade/replay/capture.hpp"

using namespace optitrade;
using namespace optitrade::replay;

namespace {

using Bytes = std::vector<std::byte>;

Bytes bytes(std::initializer_list<int> v) {
    Bytes b;
    for (int x : v) b.push_back(static_cast<std::byte>(x));
    return b;
}
void append(Bytes& dst, const Bytes& src) { dst.insert(dst.end(), src.begin(), src.end()); }

// A unique file in the system temp directory, removed on destruction.
class TempFile {
public:
    TempFile() {
        static std::atomic<unsigned> counter{0};
        path_ = (std::filesystem::temp_directory_path() /
                 ("optitrade_capture_test_" + std::to_string(::getpid()) + "_" + std::to_string(counter++) + ".bin"))
                    .string();
    }
    ~TempFile() { std::remove(path_.c_str()); }
    TempFile(const TempFile&) = delete;
    TempFile& operator=(const TempFile&) = delete;
    const char* path() const { return path_.c_str(); }

    void set(const Bytes& b) const {
        std::FILE* f = std::fopen(path_.c_str(), "wb");
        if (f == nullptr) return;
        if (!b.empty()) std::fwrite(b.data(), 1, b.size(), f);
        std::fclose(f);
    }
    Bytes get() const {
        Bytes b;
        std::FILE* f = std::fopen(path_.c_str(), "rb");
        if (f == nullptr) return b;
        std::byte chunk[4096];
        std::size_t n = 0;
        while ((n = std::fread(chunk, 1, sizeof chunk, f)) > 0) b.insert(b.end(), chunk, chunk + n);
        std::fclose(f);
        return b;
    }

private:
    std::string path_;
};

const Bytes kHeader = bytes({'O', 'T', 'C', 'A', 'P', '0', '0', '1', 1, 0, 0, 0, 0, 0, 0, 0});

// One capture record, little endian.
Bytes rec(std::uint64_t ts, const Bytes& payload) {
    Bytes b;
    for (int i = 0; i < 8; ++i) b.push_back(static_cast<std::byte>(ts >> (8 * i)));
    b.push_back(static_cast<std::byte>(payload.size() & 0xFF));
    b.push_back(static_cast<std::byte>(payload.size() >> 8));
    append(b, payload);
    return b;
}

std::span<const std::byte> view(const Bytes& b) { return {b.data(), b.size()}; }

void skip(const char* test, const char* why) { std::printf("[skip] %s: %s\n", test, why); }

// Number of open descriptors of this process, or -1 if the host offers no listing. The directory
// stream's own descriptor is open during the scan, which cancels out when two counts are compared.
int open_fd_count() {
    for (const char* dir : {"/proc/self/fd", "/dev/fd"}) {
        DIR* d = ::opendir(dir);
        if (d == nullptr) continue;
        int n = 0;
        while (::readdir(d) != nullptr) ++n;
        ::closedir(d);
        return n;
    }
    return -1;
}

// Makes the next refill of an already-open reader's stdio buffer fail with a real read(2)
// error (EISDIR): finds the descriptor behind `path` and points it at a directory. Returns false
// if the descriptor cannot be found or replaced. Data already buffered is still delivered.
bool break_reads_of(const char* path) {
    struct stat want {};
    if (::stat(path, &want) != 0) return false;
    const int dir = ::open("/", O_RDONLY);
    if (dir < 0) return false;
    bool done = false;
    for (int fd = 3; fd < 256 && !done; ++fd) {
        struct stat st {};
        if (fd != dir && ::fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_ino == want.st_ino && st.st_dev == want.st_dev) {
            done = ::dup2(dir, fd) == fd;
        }
    }
    ::close(dir);
    return done;
}

// Lowers the file size limit for its lifetime so that writes past `bytes` fail (EFBIG), the same
// way a full disk would; SIGXFSZ is ignored so the failure shows up as an error return.
class FileSizeLimit {
public:
    explicit FileSizeLimit(rlim_t bytes) {
        if (::getrlimit(RLIMIT_FSIZE, &old_) != 0) return;
        struct sigaction ign {};
        ign.sa_handler = SIG_IGN;
        sigemptyset(&ign.sa_mask);
        if (::sigaction(SIGXFSZ, &ign, &old_sa_) != 0) return;
        rlimit now = old_;
        now.rlim_cur = bytes;
        active_ = ::setrlimit(RLIMIT_FSIZE, &now) == 0;
        if (!active_) (void)::sigaction(SIGXFSZ, &old_sa_, nullptr);
    }
    ~FileSizeLimit() {
        if (!active_) return;
        (void)::setrlimit(RLIMIT_FSIZE, &old_);
        (void)::sigaction(SIGXFSZ, &old_sa_, nullptr);
    }
    FileSizeLimit(const FileSizeLimit&) = delete;
    FileSizeLimit& operator=(const FileSizeLimit&) = delete;
    bool active() const { return active_; }

private:
    rlimit old_{};
    struct sigaction old_sa_ {};
    bool active_{false};
};

// True if the host enforces the limit set by FileSizeLimit on a plain stdio stream (some
// sandboxes do not), so a test can skip instead of reporting a bug that is not there.
bool limit_is_enforced(const char* path, std::size_t limit) {
    std::FILE* f = std::fopen(path, "wb");
    if (f == nullptr) return false;
    const Bytes junk(limit * 4, std::byte{1});
    (void)std::fwrite(junk.data(), 1, junk.size(), f);
    const bool flushed = std::fflush(f) == 0;
    std::fclose(f);
    return !flushed;
}

bool same(std::span<const std::byte> a, const Bytes& b) {
    return a.size() == b.size() && (a.empty() || std::memcmp(a.data(), b.data(), a.size()) == 0);
}

}  // namespace

OT_TEST(writer_produces_the_documented_bytes) {
    TempFile f;
    {
        CaptureWriter w(f.path());
        OT_CHECK(w.ok());
        OT_CHECK(w.write(0x0102030405060708ULL, view(bytes({0xAA, 0xBB, 0xCC}))));
        OT_CHECK(w.write(1, view(bytes({0x00}))));
        OT_CHECK_EQ(w.records(), std::uint64_t{2});
        OT_CHECK(w.close());
        OT_CHECK(w.close());  // idempotent
    }
    Bytes expect = kHeader;
    append(expect, bytes({0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 0x03, 0x00, 0xAA, 0xBB, 0xCC}));
    append(expect, bytes({0x01, 0, 0, 0, 0, 0, 0, 0, 0x01, 0x00, 0x00}));
    OT_CHECK(f.get() == expect);
}

OT_TEST(reader_parses_hand_built_file) {
    TempFile f;
    Bytes file = kHeader;
    file[12] = std::byte{0x5A};  // reserved bytes are ignored
    append(file, bytes({0xFF, 0xEE, 0xDD, 0xCC, 0xBB, 0xAA, 0x00, 0x80, 0x02, 0x00, 0x11, 0x22}));
    append(file, rec(42, bytes({0x33})));
    f.set(file);

    CaptureReader r(f.path());
    OT_CHECK(r.ok());
    Record x;
    OT_CHECK(r.next(x));
    OT_CHECK_EQ(x.ts, std::uint64_t{0x8000'AABB'CCDD'EEFFULL});
    OT_CHECK(same(x.payload, bytes({0x11, 0x22})));
    OT_CHECK(r.next(x));
    OT_CHECK_EQ(x.ts, std::uint64_t{42});
    OT_CHECK(same(x.payload, bytes({0x33})));
    OT_CHECK(!r.next(x));
    OT_CHECK(r.error() == DecodeStatus::ok);  // clean end of file
    OT_CHECK(!r.next(x));                     // and stays ended
    OT_CHECK_EQ(r.records_read(), std::uint64_t{2});
}

OT_TEST(round_trip_100k_random_records) {
    TempFile f;
    constexpr std::uint64_t kN = 100'000;
    {
        CaptureWriter w(f.path());
        OT_CHECK(w.ok());
        Rng gen(77);
        std::array<std::byte, 64> buf{};
        for (std::uint64_t i = 0; i < kN; ++i) {
            const std::size_t len = 1 + static_cast<std::size_t>(gen.bounded(64));
            for (std::size_t k = 0; k < len; ++k) buf[k] = static_cast<std::byte>(gen.next());
            OT_CHECK(w.write(gen.next(), std::span<const std::byte>(buf.data(), len)));
        }
        OT_CHECK_EQ(w.records(), kN);
        OT_CHECK(w.close());
    }
    CaptureReader r(f.path());
    OT_CHECK(r.ok());
    Rng gen(77);  // replays the same sequence to build the expectation
    std::array<std::byte, 64> want{};
    std::uint64_t n = 0, mismatches = 0;
    Record x;
    while (r.next(x)) {
        const std::size_t len = 1 + static_cast<std::size_t>(gen.bounded(64));
        for (std::size_t k = 0; k < len; ++k) want[k] = static_cast<std::byte>(gen.next());
        const std::uint64_t ts = gen.next();
        if (x.ts != ts || x.payload.size() != len || std::memcmp(x.payload.data(), want.data(), len) != 0) {
            ++mismatches;
        }
        ++n;
    }
    OT_CHECK_EQ(n, kN);
    OT_CHECK_EQ(mismatches, std::uint64_t{0});
    OT_CHECK(r.error() == DecodeStatus::ok);
    OT_CHECK_EQ(r.records_read(), kN);
}

OT_TEST(truncation_at_every_position_is_detected) {
    TempFile f;
    Bytes full = kHeader;
    append(full, rec(1, bytes({1, 2, 3, 4, 5})));
    append(full, rec(2, bytes({6, 7, 8})));
    const std::size_t first_end = kHeader.size() + 10 + 5;

    for (std::size_t cut = 0; cut < full.size(); ++cut) {
        f.set(Bytes(full.begin(), full.begin() + static_cast<std::ptrdiff_t>(cut)));
        CaptureReader r(f.path());
        Record x;
        std::uint64_t got = 0;
        if (cut < kHeader.size()) {
            OT_CHECK(!r.ok());  // inside the header
            OT_CHECK(r.error() == DecodeStatus::truncated);
            OT_CHECK(!r.next(x));
            continue;
        }
        OT_CHECK(r.ok());
        while (r.next(x)) ++got;
        const std::uint64_t whole = cut >= first_end ? 1 : 0;
        OT_CHECK_EQ(got, whole);
        const bool on_boundary = cut == kHeader.size() || cut == first_end;
        if (on_boundary) {
            OT_CHECK(r.error() == DecodeStatus::ok);  // a clean end, not damage
        } else {
            OT_CHECK(r.error() == DecodeStatus::truncated);
            OT_CHECK(!r.next(x));  // sticky
        }
    }
    f.set(full);
    CaptureReader r(f.path());
    Record x;
    OT_CHECK(r.next(x));
    OT_CHECK(r.next(x));
    OT_CHECK(!r.next(x));
    OT_CHECK(r.error() == DecodeStatus::ok);
}

OT_TEST(bad_magic_wrong_version_and_empty_or_missing_file) {
    TempFile f;
    Bytes bad_magic = kHeader;
    bad_magic[7] = std::byte{'2'};  // "OTCAP002"
    f.set(bad_magic);
    {
        CaptureReader r(f.path());
        Record x;
        OT_CHECK(!r.ok());
        OT_CHECK(r.error() == DecodeStatus::unknown_type);
        OT_CHECK(!r.next(x));
    }
    for (int version : {0, 2, 0x01000000}) {
        Bytes b = kHeader;
        b[8] = static_cast<std::byte>(version & 0xFF);
        b[9] = static_cast<std::byte>((version >> 8) & 0xFF);
        b[10] = static_cast<std::byte>((version >> 16) & 0xFF);
        b[11] = static_cast<std::byte>((version >> 24) & 0xFF);
        f.set(b);
        CaptureReader r(f.path());
        Record x;
        OT_CHECK(!r.ok());
        OT_CHECK(r.error() == DecodeStatus::bad_field);
        OT_CHECK(!r.next(x));
    }
    f.set({});
    {
        CaptureReader r(f.path());
        Record x;
        OT_CHECK(!r.ok());
        OT_CHECK(r.error() == DecodeStatus::truncated);
        OT_CHECK(!r.next(x));
    }
    f.set(kHeader);  // header only: valid, zero records
    {
        CaptureReader r(f.path());
        Record x;
        OT_CHECK(r.ok());
        OT_CHECK(!r.next(x));
        OT_CHECK(r.error() == DecodeStatus::ok);
    }
    std::remove(f.path());
    CaptureReader missing(f.path());
    Record x;
    OT_CHECK(!missing.ok());
    OT_CHECK(!missing.next(x));
}

OT_TEST(missing_files_report_truncated) {
    TempFile f;  // never created
    CaptureReader c(f.path());
    ItchFileReader i(f.path());
    Record x;
    OT_CHECK(!c.ok());
    OT_CHECK(c.error() == DecodeStatus::truncated);
    OT_CHECK(!c.next(x));
    OT_CHECK_EQ(c.records_read(), std::uint64_t{0});
    OT_CHECK(!i.ok());
    OT_CHECK(i.error() == DecodeStatus::truncated);
    OT_CHECK(!i.next(x));
    OT_CHECK_EQ(i.records_read(), std::uint64_t{0});
}

OT_TEST(capture_payload_limit_is_exactly_kCaptureMaxPayload) {
    static_assert(kCaptureMaxPayload == 4096, "the on-disk format promises this bound");
    TempFile f;
    const Bytes at_limit(kCaptureMaxPayload, std::byte{0x5A});
    {
        CaptureWriter w(f.path());
        OT_CHECK(w.write(1, view(at_limit)));
        OT_CHECK(!w.write(2, view(Bytes(kCaptureMaxPayload + 1, std::byte{0x5A}))));
        OT_CHECK(w.ok());  // refusing an oversized payload is not an I/O failure
        OT_CHECK(w.write(3, view(bytes({0x01}))));
        OT_CHECK_EQ(w.records(), std::uint64_t{2});
        OT_CHECK(w.close());
    }
    Bytes expect = kHeader;
    append(expect, rec(1, at_limit));
    append(expect, rec(3, bytes({0x01})));
    OT_CHECK(f.get() == expect);

    CaptureReader r(f.path());
    Record x;
    OT_CHECK(r.next(x));
    OT_CHECK(same(x.payload, at_limit));
    OT_CHECK(r.next(x));
    OT_CHECK(!r.next(x));
    OT_CHECK(r.error() == DecodeStatus::ok);
}

OT_TEST(a_directory_is_not_a_clean_empty_capture) {
    // fopen(dir, "rb") succeeds on Linux and macOS; the first read(2) then fails with EISDIR.
    const std::string dir = std::filesystem::temp_directory_path().string();
    CaptureReader c(dir.c_str());
    Record x;
    OT_CHECK(!c.ok());
    OT_CHECK(c.error() != DecodeStatus::ok);
    OT_CHECK(!c.next(x));
    ItchFileReader i(dir.c_str());
    OT_CHECK(!i.next(x));
    OT_CHECK(i.error() != DecodeStatus::ok);  // a failed read must not look like end of file
}

OT_TEST(readers_release_their_file_descriptors) {
    const int before = open_fd_count();
    if (before < 0) {
        skip("readers_release_their_file_descriptors", "no /proc/self/fd or /dev/fd");
        return;
    }
    TempFile good;
    TempFile bad;
    TempFile out;
    {
        Bytes file = kHeader;
        append(file, rec(1, bytes({1, 2, 3})));
        good.set(file);
        Bytes junk = kHeader;
        junk[0] = std::byte{'X'};  // header rejected: the file is open but the reader is not ok
        bad.set(junk);
    }
    Record x;
    for (int i = 0; i < 300; ++i) {  // more than the default 256-descriptor limit on macOS
        CaptureReader c(good.path());
        OT_CHECK(c.ok() && c.next(x));
        CaptureReader b(bad.path());
        OT_CHECK(!b.ok());
        ItchFileReader t(good.path());
        OT_CHECK(t.ok());
        CaptureWriter w(out.path());
        OT_CHECK(w.ok());
    }
    OT_CHECK_EQ(open_fd_count(), before);
}

OT_TEST(zero_length_and_oversized_payloads_are_rejected) {
    TempFile f;
    // Reader: a stored zero-length record and a length above the maximum both stop the read.
    Bytes zero = kHeader;
    append(zero, rec(1, bytes({9})));
    append(zero, rec(2, {}));
    append(zero, rec(3, bytes({9})));
    f.set(zero);
    {
        CaptureReader r(f.path());
        Record x;
        OT_CHECK(r.next(x));
        OT_CHECK(!r.next(x));
        OT_CHECK(r.error() == DecodeStatus::bad_length);
        OT_CHECK(!r.next(x));
        OT_CHECK_EQ(r.records_read(), std::uint64_t{1});
    }
    Bytes big = kHeader;
    append(big, rec(1, Bytes(kCaptureMaxPayload, std::byte{7})));  // exactly the maximum: accepted
    append(big, rec(2, Bytes(kCaptureMaxPayload + 1, std::byte{7})));
    f.set(big);
    {
        CaptureReader r(f.path());
        Record x;
        OT_CHECK(r.next(x));
        OT_CHECK_EQ(x.payload.size(), kCaptureMaxPayload);
        OT_CHECK(!r.next(x));
        OT_CHECK(r.error() == DecodeStatus::bad_length);
    }
    Bytes huge = kHeader;  // a corrupt 0xFFFF length must not be followed
    append(huge, bytes({0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF}));
    append(huge, Bytes(70'000, std::byte{1}));
    f.set(huge);
    {
        CaptureReader r(f.path());
        Record x;
        OT_CHECK(!r.next(x));
        OT_CHECK(r.error() == DecodeStatus::bad_length);
    }

    // Writer: the same payloads are refused without harming the file.
    {
        CaptureWriter w(f.path());
        OT_CHECK(!w.write(1, {}));
        OT_CHECK(!w.write(2, view(Bytes(kCaptureMaxPayload + 1, std::byte{1}))));
        OT_CHECK(w.ok());
        OT_CHECK_EQ(w.records(), std::uint64_t{0});
        OT_CHECK(w.write(3, view(Bytes(kCaptureMaxPayload, std::byte{1}))));
        OT_CHECK(w.write(4, view(bytes({1}))));
        OT_CHECK(w.close());
    }
    CaptureReader r(f.path());
    Record x;
    OT_CHECK(r.next(x));
    OT_CHECK_EQ(x.ts, std::uint64_t{3});
    OT_CHECK(r.next(x));
    OT_CHECK_EQ(x.ts, std::uint64_t{4});
    OT_CHECK(!r.next(x));
    OT_CHECK(r.error() == DecodeStatus::ok);
}

OT_TEST(writer_reports_unwritable_path) {
    CaptureWriter w("/nonexistent_directory_for_optitrade_tests/x.otcap");
    OT_CHECK(!w.ok());
    OT_CHECK(!w.write(1, view(bytes({1}))));
    OT_CHECK(!w.close());
    OT_CHECK_EQ(w.records(), std::uint64_t{0});
}

OT_TEST(writer_ok_turns_false_after_close_and_writes_are_refused) {
    TempFile f;
    CaptureWriter w(f.path());
    OT_CHECK(w.ok());
    OT_CHECK(w.write(1, view(bytes({1}))));
    OT_CHECK(w.close());
    OT_CHECK(!w.ok());  // the file is gone, nothing more can be written
    OT_CHECK(!w.write(2, view(bytes({2}))));
    OT_CHECK_EQ(w.records(), std::uint64_t{1});
    OT_CHECK(w.close());  // still reports the first close's success
}

OT_TEST(close_reports_a_failure_that_only_surfaces_when_flushing) {
    TempFile f;
    if (!FileSizeLimit(1).active()) {
        skip("close_reports_a_failure_that_only_surfaces_when_flushing", "setrlimit(RLIMIT_FSIZE) unavailable");
        return;
    }
    constexpr std::size_t kLimit = 1000;
    {
        FileSizeLimit limit(kLimit);
        if (!limit_is_enforced(f.path(), kLimit)) {
            skip("close_reports_a_failure_that_only_surfaces_when_flushing", "host does not enforce RLIMIT_FSIZE");
            return;
        }
        // 200 records of 50 bytes sit in the 64 KiB stdio buffer, so every write() succeeds
        // and the out-of-space error first shows up when close() flushes.
        CaptureWriter w(f.path());
        OT_CHECK(w.ok());
        std::uint64_t accepted = 0;
        for (std::uint64_t i = 0; i < 200; ++i) accepted += w.write(i, view(Bytes(40, std::byte{1}))) ? 1 : 0;
        OT_CHECK_EQ(accepted, std::uint64_t{200});
        OT_CHECK(w.ok());
        OT_CHECK(!w.close());  // the data did not reach the file: must not claim success
        OT_CHECK(!w.ok());
        OT_CHECK(!w.close());  // and the verdict is stable
    }
    // The same failure hit mid-stream: a write() that overflows the buffer fails and sticks.
    {
        FileSizeLimit limit(kLimit);
        CaptureWriter w(f.path());
        OT_CHECK(w.ok());
        const Bytes big(kCaptureMaxPayload, std::byte{2});
        bool failed = false;
        for (int i = 0; i < 40 && !failed; ++i) failed = !w.write(static_cast<Nanos>(i), view(big));
        OT_CHECK(failed);
        OT_CHECK(!w.ok());
        OT_CHECK(!w.write(99, view(bytes({1}))));
        OT_CHECK(!w.close());
    }
}

namespace {

// Hand-built 'H' Stock Trading Action (25 bytes): a valid ITCH type this library does not model.
Bytes trading_action(std::uint64_t ts) {
    Bytes m(25, std::byte{' '});
    m[0] = std::byte{'H'};
    m[1] = std::byte{0};
    m[2] = std::byte{1};  // locate 1
    m[3] = std::byte{0};
    m[4] = std::byte{0};
    for (int i = 0; i < 6; ++i) m[5 + static_cast<std::size_t>(i)] = static_cast<std::byte>(ts >> (8 * (5 - i)));
    return m;
}

Bytes framed(const Bytes& m) {
    Bytes b = bytes({static_cast<int>(m.size() >> 8), static_cast<int>(m.size() & 0xFF)});
    append(b, m);
    return b;
}

template <class M>
Bytes framed_itch(const M& m) {
    std::array<std::byte, 64> buf{};
    const std::size_t n = itch::encode_framed(m, buf);
    return Bytes(buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(n));
}

}  // namespace

OT_TEST(itch_file_reader_returns_every_frame_with_header_timestamps) {
    TempFile f;
    itch::AddOrder add;
    add.h = {7, 0, 0x0000'1234'5678'9ABCULL};  // needs more than 32 bits
    add.ref = 99;
    add.side = Side::sell;
    add.shares = 300;
    add.symbol = Symbol("ABC");
    add.price = 1'234'500;
    itch::OrderDelete del{{7, 0, 0x0000'FFFF'FFFF'FFFFULL}, 99};

    Bytes file;
    append(file, framed_itch(add));
    append(file, framed(trading_action(0x0000'0000'0000'0BEEULL)));  // unsupported type, passed through
    append(file, bytes({0, 0}));                                     // zero-length frame: dropped
    append(file, framed(bytes({'Z', 1, 2, 3, 4})));                  // shorter than a header: inherits the timestamp
    append(file, framed_itch(del));
    f.set(file);

    ItchFileReader r(f.path());
    OT_CHECK(r.ok());
    Record x;

    OT_CHECK(r.next(x));
    OT_CHECK_EQ(x.ts, std::uint64_t{0x0000'1234'5678'9ABCULL});
    OT_CHECK_EQ(x.payload.size(), itch::kAddOrderLength);
    OT_CHECK_EQ(std::to_integer<int>(x.payload[0]), int{'A'});
    struct Grab : itch::NullHandler {
        using itch::NullHandler::on;
        itch::AddOrder add;
        void on(const itch::AddOrder& m) noexcept { add = m; }
    } grab;
    OT_CHECK(itch::decode(x.payload, grab) == DecodeStatus::ok);
    OT_CHECK(grab.add == add);

    OT_CHECK(r.next(x));
    OT_CHECK_EQ(x.ts, std::uint64_t{0xBEE});
    OT_CHECK_EQ(x.payload.size(), std::size_t{25});
    itch::NullHandler null;
    OT_CHECK(itch::decode(x.payload, null) == DecodeStatus::unknown_type);  // skipping is the decoder's call

    OT_CHECK(r.next(x));
    OT_CHECK_EQ(x.payload.size(), std::size_t{5});
    OT_CHECK_EQ(x.ts, std::uint64_t{0xBEE});

    OT_CHECK(r.next(x));
    OT_CHECK_EQ(x.ts, std::uint64_t{0x0000'FFFF'FFFF'FFFFULL});
    OT_CHECK_EQ(x.payload.size(), itch::kOrderDeleteLength);

    OT_CHECK(!r.next(x));
    OT_CHECK(r.error() == DecodeStatus::ok);
    OT_CHECK_EQ(r.records_read(), std::uint64_t{4});
}

OT_TEST(itch_file_reader_timestamp_needs_a_full_eleven_byte_header) {
    // The timestamp occupies bytes 5..10, so it takes 11 bytes to hold it: a 10-byte frame has
    // no complete timestamp and must inherit the previous one rather than read past its end.
    TempFile f;
    const auto stamp = [](std::uint64_t ts, std::size_t len) {
        Bytes m(len, std::byte{0xFF});  // 0xFF filler: a misplaced read shows up in the value
        for (int i = 0; i < 6; ++i) m[5 + static_cast<std::size_t>(i)] = static_cast<std::byte>(ts >> (8 * (5 - i)));
        return m;
    };
    Bytes file;
    append(file, framed(Bytes(10, std::byte{0xFF})));  // first frame, too short: time stays 0
    append(file, framed(stamp(0x1122'3344'5566ULL, 11)));  // exactly long enough
    append(file, framed(Bytes(10, std::byte{0xFF})));  // inherits 0x112233445566
    append(file, framed(stamp(0x0000'0000'0042ULL, 11)));
    append(file, framed(Bytes(1, std::byte{'Z'})));    // 1 byte: same rule
    append(file, framed(stamp(0x0000'0000'0043ULL, 12)));
    f.set(file);

    ItchFileReader r(f.path());
    Record x;
    const std::pair<std::size_t, std::uint64_t> want[] = {{10, 0}, {11, 0x1122'3344'5566ULL}, {10, 0x1122'3344'5566ULL},
                                                        {11, 0x42},  {1, 0x42},                  {12, 0x43}};
    for (const auto& [len, ts] : want) {
        OT_CHECK(r.next(x));
        OT_CHECK_EQ(x.payload.size(), len);
        OT_CHECK_EQ(x.ts, ts);
    }
    OT_CHECK(!r.next(x));
    OT_CHECK(r.error() == DecodeStatus::ok);
    OT_CHECK_EQ(r.records_read(), std::uint64_t{6});
}

OT_TEST(read_error_at_a_record_boundary_is_not_a_clean_end) {
    // The stdio buffer is 64 KiB, so a file whose first 65536 bytes end exactly on a record
    // boundary makes the failing refill land where the readers decide between "clean end" and
    // "damage" (fread returns 0 in both cases; only feof tells them apart).
    TempFile f;
    {
        Bytes file = kHeader;  // 16 + 1365 * (10 + 38) = 65536
        for (int k = 0; k < 1365 + 50; ++k) append(file, rec(static_cast<std::uint64_t>(k), Bytes(38, std::byte{3})));
        f.set(file);
        CaptureReader r(f.path());
        Record x;
        OT_CHECK(r.ok());
        if (!break_reads_of(f.path())) {
            skip("read_error_at_a_record_boundary_is_not_a_clean_end", "cannot find the reader's descriptor");
            return;
        }
        std::uint64_t got = 0;
        while (r.next(x)) ++got;
        OT_CHECK(got <= 1365);  // only what was already buffered
        OT_CHECK(r.error() != DecodeStatus::ok);
        OT_CHECK(!r.next(x));
    }
    {
        Bytes file;  // 2048 * (2 + 30) = 65536
        for (int k = 0; k < 2048 + 50; ++k) append(file, framed(Bytes(30, std::byte{3})));
        f.set(file);
        ItchFileReader r(f.path());
        Record x;
        OT_CHECK(r.next(x));  // forces the first buffer fill; 2047 frames are still buffered
        if (!break_reads_of(f.path())) {
            skip("read_error_at_a_record_boundary_is_not_a_clean_end", "cannot find the reader's descriptor");
            return;
        }
        std::uint64_t got = 1;
        while (r.next(x)) ++got;
        OT_CHECK(got <= 2048);
        OT_CHECK(r.error() != DecodeStatus::ok);
    }
}

OT_TEST(itch_file_reader_handles_largest_frame_empty_and_truncated_files) {
    TempFile f;
    Bytes big(0xFFFF, std::byte{0x5A});
    big[0] = std::byte{'Q'};
    Bytes file = framed(big);
    // framed() stores the size in two bytes: 0xFFFF fits exactly.
    f.set(file);
    {
        ItchFileReader r(f.path());
        Record x;
        OT_CHECK(r.next(x));
        OT_CHECK_EQ(x.payload.size(), std::size_t{0xFFFF});
        OT_CHECK(!r.next(x));
        OT_CHECK(r.error() == DecodeStatus::ok);
    }

    f.set({});
    {
        ItchFileReader r(f.path());
        Record x;
        OT_CHECK(r.ok());
        OT_CHECK(!r.next(x));
        OT_CHECK(r.error() == DecodeStatus::ok);
    }

    Bytes two = framed_itch(itch::OrderDelete{{1, 0, 5}, 1});
    append(two, framed_itch(itch::OrderDelete{{1, 0, 6}, 2}));
    for (std::size_t cut = 0; cut < two.size(); ++cut) {
        f.set(Bytes(two.begin(), two.begin() + static_cast<std::ptrdiff_t>(cut)));
        ItchFileReader r(f.path());
        Record x;
        std::uint64_t got = 0;
        while (r.next(x)) ++got;
        const std::size_t one = 2 + itch::kOrderDeleteLength;
        OT_CHECK_EQ(got, std::uint64_t{cut >= one ? 1u : 0u});
        const bool boundary = cut == 0 || cut == one;
        OT_CHECK(r.error() == (boundary ? DecodeStatus::ok : DecodeStatus::truncated));
    }

    std::remove(f.path());
    ItchFileReader missing(f.path());
    Record x;
    OT_CHECK(!missing.ok());
    OT_CHECK(!missing.next(x));
}

OT_TEST(memory_source_replays_rewinds_and_owns_its_bytes) {
    MemorySource src;
    OT_CHECK_EQ(src.size(), std::size_t{0});
    Record x;
    OT_CHECK(!src.next(x));

    Bytes a = bytes({1, 2, 3});
    src.add(10, view(a));
    a[0] = std::byte{9};  // the source copied its input
    src.add(20, view(bytes({4})));
    src.add(30, {});
    OT_CHECK_EQ(src.size(), std::size_t{3});

    for (int pass = 0; pass < 2; ++pass) {
        OT_CHECK(src.next(x));
        OT_CHECK_EQ(x.ts, std::uint64_t{10});
        OT_CHECK(same(x.payload, bytes({1, 2, 3})));
        OT_CHECK(src.next(x));
        OT_CHECK_EQ(x.ts, std::uint64_t{20});
        OT_CHECK(same(x.payload, bytes({4})));
        OT_CHECK(src.next(x));
        OT_CHECK_EQ(x.ts, std::uint64_t{30});
        OT_CHECK_EQ(x.payload.size(), std::size_t{0});
        OT_CHECK(!src.next(x));
        OT_CHECK(!src.next(x));
        src.rewind();
    }

    // Records added after a partial read do not disturb the position.
    src.next(x);
    src.add(40, view(bytes({7, 7})));
    OT_CHECK(src.next(x));
    OT_CHECK_EQ(x.ts, std::uint64_t{20});
    OT_CHECK_EQ(src.size(), std::size_t{4});
}

OT_TEST_MAIN()
