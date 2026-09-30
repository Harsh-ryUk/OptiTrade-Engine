#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <span>
#include <vector>

#include "optitrade/core/decode_status.hpp"
#include "optitrade/core/endian.hpp"
#include "optitrade/core/types.hpp"

// Sources of timestamped ITCH messages for replay and backtesting: the project's
// own capture format, Nasdaq BinaryFILE files, and an in-memory list.
//
// All file readers work from fixed buffers owned by the object (an stdio buffer
// and one payload buffer), so reading a file performs no allocation per record.
// A record's payload is a view into that buffer and is valid until the next call
// to next().
//
// Capture file "OTCAP001" (little endian, unlike the big-endian wire formats):
//   header  : magic "OTCAP001" (8) | version u32 = 1 | reserved u32 (written as 0,
//             ignored on read so a later writer may use it)
//   record  : ts u64 | payload length u16 | payload (one unframed ITCH message)
// A payload must be 1..kCaptureMaxPayload bytes. The bound is far above the longest
// ITCH message (44 bytes); it exists so that a corrupted length cannot make a reader
// swallow tens of kilobytes of garbage before noticing.
namespace optitrade::replay {

struct Record {
    Nanos ts{};
    std::span<const std::byte> payload;  // valid until the next next()
};

inline constexpr std::size_t kCaptureHeaderSize = 16;
inline constexpr std::size_t kCaptureRecordHeaderSize = 10;
inline constexpr std::size_t kCaptureMaxPayload = 4096;
inline constexpr std::uint32_t kCaptureVersion = 1;
inline constexpr std::array<char, 8> kCaptureMagic = {'O', 'T', 'C', 'A', 'P', '0', '0', '1'};

namespace detail {

inline constexpr std::size_t kIoBufferSize = 1u << 16;

// Attaches a caller-owned buffer to a stream (before any I/O, as stdio requires).
inline void use_buffer(std::FILE* f, char* buf, std::size_t size) noexcept {
    if (f != nullptr) std::setvbuf(f, buf, _IOFBF, size);
}

}  // namespace detail

// Writes a capture file. Bytes go through a 64 KiB buffer; close() (or the
// destructor) flushes it. The object owns its stdio buffer, hence it is not movable.
class CaptureWriter {
public:
    explicit CaptureWriter(const char* path) noexcept {
        file_ = std::fopen(path, "wb");
        detail::use_buffer(file_, io_buf_.data(), io_buf_.size());
        if (file_ == nullptr) return;
        std::array<std::byte, kCaptureHeaderSize> hdr{};
        std::memcpy(hdr.data(), kCaptureMagic.data(), kCaptureMagic.size());
        le::store32(hdr.data() + 8, kCaptureVersion);
        ok_ = std::fwrite(hdr.data(), 1, hdr.size(), file_) == hdr.size();
    }
    CaptureWriter(const CaptureWriter&) = delete;
    CaptureWriter& operator=(const CaptureWriter&) = delete;
    ~CaptureWriter() { close(); }

    // True while the file is open and no I/O error has occurred.
    bool ok() const noexcept { return file_ != nullptr && ok_; }

    // Appends one record. Returns false, writing nothing, for an empty or oversized
    // payload (a caller error that leaves the file valid); returns false and marks
    // the writer failed on an I/O error.
    bool write(Nanos ts, std::span<const std::byte> payload) noexcept {
        if (!ok() || payload.empty() || payload.size() > kCaptureMaxPayload) return false;
        std::array<std::byte, kCaptureRecordHeaderSize> hdr{};
        le::store64(hdr.data(), ts);
        le::store16(hdr.data() + 8, static_cast<std::uint16_t>(payload.size()));
        if (std::fwrite(hdr.data(), 1, hdr.size(), file_) != hdr.size() ||
            std::fwrite(payload.data(), 1, payload.size(), file_) != payload.size()) {
            ok_ = false;
            return false;
        }
        ++records_;
        return true;
    }

    // Flushes and closes. Returns true only if every byte reached the operating
    // system; idempotent (later calls return the first result's state).
    bool close() noexcept {
        if (file_ != nullptr) {
            const bool flushed = std::fflush(file_) == 0;
            const bool closed = std::fclose(file_) == 0;
            file_ = nullptr;
            ok_ = ok_ && flushed && closed;
        }
        return ok_;
    }

    std::uint64_t records() const noexcept { return records_; }

private:
    std::FILE* file_{nullptr};
    bool ok_{false};
    std::uint64_t records_{0};
    std::array<char, detail::kIoBufferSize> io_buf_;
};

// Reads a capture file. Failures are sticky and reported through error():
//   truncated     no complete header (missing, empty or short file), or a record cut
//                 short in its header or payload
//   unknown_type  the magic is not "OTCAP001"
//   bad_field     unsupported version
//   bad_length    a record with zero or more than kCaptureMaxPayload payload bytes
// A file that ends exactly on a record boundary is a normal end: next() returns
// false and error() stays ok. ok() is false if the file could not be opened or the
// header was rejected, and stays true if a later record is bad (see error()).
class CaptureReader {
public:
    explicit CaptureReader(const char* path) noexcept {
        file_ = std::fopen(path, "rb");
        detail::use_buffer(file_, io_buf_.data(), io_buf_.size());
        if (file_ == nullptr) {
            error_ = DecodeStatus::truncated;  // nothing to read: the same outcome as an empty file
            return;
        }
        std::array<std::byte, kCaptureHeaderSize> hdr{};
        if (std::fread(hdr.data(), 1, hdr.size(), file_) != hdr.size()) {
            error_ = DecodeStatus::truncated;
        } else if (std::memcmp(hdr.data(), kCaptureMagic.data(), kCaptureMagic.size()) != 0) {
            error_ = DecodeStatus::unknown_type;
        } else if (le::load32(hdr.data() + 8) != kCaptureVersion) {
            error_ = DecodeStatus::bad_field;
        } else {
            header_ok_ = true;
        }
    }
    CaptureReader(const CaptureReader&) = delete;
    CaptureReader& operator=(const CaptureReader&) = delete;
    ~CaptureReader() {
        if (file_ != nullptr) std::fclose(file_);
    }

    bool ok() const noexcept { return header_ok_; }

    // Next record, or false at the end of the file or on the first error.
    bool next(Record& out) noexcept {
        if (!header_ok_ || error_ != DecodeStatus::ok) return false;
        std::array<std::byte, kCaptureRecordHeaderSize> hdr{};
        const std::size_t got = std::fread(hdr.data(), 1, hdr.size(), file_);
        if (got == 0 && std::feof(file_) != 0) return false;  // clean end
        if (got != hdr.size()) return fail(DecodeStatus::truncated);
        const std::uint16_t len = le::load16(hdr.data() + 8);
        if (len == 0 || len > kCaptureMaxPayload) return fail(DecodeStatus::bad_length);
        if (std::fread(payload_.data(), 1, len, file_) != len) return fail(DecodeStatus::truncated);
        out.ts = le::load64(hdr.data());
        out.payload = std::span<const std::byte>(payload_.data(), len);
        ++records_;
        return true;
    }

    DecodeStatus error() const noexcept { return error_; }
    std::uint64_t records_read() const noexcept { return records_; }

private:
    bool fail(DecodeStatus s) noexcept {
        error_ = s;
        return false;
    }

    std::FILE* file_{nullptr};
    bool header_ok_{false};
    DecodeStatus error_{DecodeStatus::ok};
    std::uint64_t records_{0};
    std::array<std::byte, kCaptureMaxPayload> payload_;
    std::array<char, detail::kIoBufferSize> io_buf_;
};

// Reads a Nasdaq BinaryFILE ITCH file: [u16 big-endian length][message] repeated.
//
// The reader is deliberately ignorant of message types. It returns EVERY framed
// message, including types this library does not model (trading actions, NOII, ...)
// and malformed ones, and leaves skipping and validation to the decoder, which is
// where those decisions live (itch::decode reports unknown_type for them). Only
// zero-length frames, which carry nothing, are dropped. The record timestamp is the
// message's own 48-bit timestamp (bytes 5..10; every ITCH 5.0 message has one). A
// frame shorter than 11 bytes cannot carry it and inherits the previous timestamp
// (0 at the start), which keeps replay time monotonic without inventing data.
//
// error(): truncated if the file ends inside a frame (the incomplete frame is not
// returned) or could not be opened; a file that ends on a frame boundary is a normal
// end and error() stays ok.
class ItchFileReader {
public:
    static constexpr std::size_t kMaxFrame = 0xFFFF;

    explicit ItchFileReader(const char* path) noexcept {
        file_ = std::fopen(path, "rb");
        detail::use_buffer(file_, io_buf_.data(), io_buf_.size());
        if (file_ == nullptr) error_ = DecodeStatus::truncated;
    }
    ItchFileReader(const ItchFileReader&) = delete;
    ItchFileReader& operator=(const ItchFileReader&) = delete;
    ~ItchFileReader() {
        if (file_ != nullptr) std::fclose(file_);
    }

    bool ok() const noexcept { return file_ != nullptr; }

    bool next(Record& out) noexcept {
        if (file_ == nullptr || error_ != DecodeStatus::ok) return false;
        for (;;) {
            std::array<std::byte, 2> hdr{};
            const std::size_t got = std::fread(hdr.data(), 1, hdr.size(), file_);
            if (got == 0 && std::feof(file_) != 0) return false;  // clean end
            if (got != hdr.size()) return fail();
            const std::size_t len = be::load16(hdr.data());
            if (len == 0) continue;
            if (std::fread(payload_.data(), 1, len, file_) != len) return fail();
            if (len >= 11) last_ts_ = be::load48(payload_.data() + 5);
            out.ts = last_ts_;
            out.payload = std::span<const std::byte>(payload_.data(), len);
            ++records_;
            return true;
        }
    }

    DecodeStatus error() const noexcept { return error_; }
    std::uint64_t records_read() const noexcept { return records_; }

private:
    bool fail() noexcept {
        error_ = DecodeStatus::truncated;
        return false;
    }

    std::FILE* file_{nullptr};
    DecodeStatus error_{DecodeStatus::ok};
    std::uint64_t records_{0};
    Nanos last_ts_{0};
    std::array<std::byte, kMaxFrame> payload_;
    std::array<char, detail::kIoBufferSize> io_buf_;
};

// Pre-generated records held in memory (tests, benchmarks): replaying costs no I/O
// and no allocation. add() may allocate; call it before the timed loop.
class MemorySource {
public:
    void add(Nanos ts, std::span<const std::byte> payload) {
        entries_.push_back({ts, arena_.size(), payload.size()});
        arena_.insert(arena_.end(), payload.begin(), payload.end());
    }

    bool next(Record& out) noexcept {
        if (cursor_ >= entries_.size()) return false;
        const Entry& e = entries_[cursor_++];
        out.ts = e.ts;
        out.payload = std::span<const std::byte>(arena_.data() + e.offset, e.length);
        return true;
    }

    void rewind() noexcept { cursor_ = 0; }
    std::size_t size() const noexcept { return entries_.size(); }

private:
    struct Entry {
        Nanos ts;
        std::size_t offset;
        std::size_t length;
    };
    std::vector<Entry> entries_;
    std::vector<std::byte> arena_;  // all payloads back to back; offsets survive growth
    std::size_t cursor_{0};
};

}  // namespace optitrade::replay
