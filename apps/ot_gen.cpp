// ot_gen: writes a deterministic synthetic ITCH 5.0 market to an OTCAP001 capture file.
//
// The output is a pure function of (seed, symbols, messages): the same arguments
// produce a byte-identical file on every platform. The printed digest makes that
// checkable without diffing files. It is the 64-bit FNV-1a over the record stream
// (per record: timestamp as 8 little-endian bytes, then the payload bytes), so it
// does not depend on the file container.

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <system_error>

#include "optitrade/core/digest.hpp"
#include "optitrade/itch/messages.hpp"
#include "optitrade/replay/capture.hpp"
#include "optitrade/sim/synthetic_market.hpp"

namespace {

using namespace optitrade;

constexpr int kExitUsage = 2;

void usage(std::FILE* out) {
    std::fputs(
        "usage: ot_gen --out FILE.otcap [--seed N] [--symbols N] [--messages N]\n"
        "\n"
        "Writes a deterministic synthetic ITCH 5.0 market to an OTCAP001 capture file.\n"
        "\n"
        "  --out FILE     output capture file (required; overwritten if it exists)\n"
        "  --seed N       random seed (default 1)\n"
        "  --symbols N    number of instruments, 1..65535 (default 8)\n"
        "  --messages N   flow messages after the fixed preamble (default 100000)\n"
        "  --help         show this text\n"
        "\n"
        "Prints the record count, the file size in bytes and the FNV-1a digest of the\n"
        "record stream (timestamp and payload of every record).\n",
        out);
}

// Whole-string unsigned decimal in [0, max]. Rejects signs, blanks, trailing junk
// and overflow, all of which strtoull would otherwise accept silently.
bool parse_u64(const char* text, std::uint64_t max, std::uint64_t& out) {
    if (text[0] < '0' || text[0] > '9') return false;
    errno = 0;
    char* end = nullptr;
    const unsigned long long v = std::strtoull(text, &end, 10);
    if (errno != 0 || *end != '\0' || v > max) return false;
    out = v;
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    sim::SyntheticConfig cfg;
    const char* out_path = nullptr;

    for (int i = 1; i < argc; ++i) {
        const char* arg = argv[i];
        if (std::strcmp(arg, "--help") == 0 || std::strcmp(arg, "-h") == 0) {
            usage(stdout);
            return 0;
        }
        const bool is_seed = std::strcmp(arg, "--seed") == 0;
        const bool is_symbols = std::strcmp(arg, "--symbols") == 0;
        const bool is_messages = std::strcmp(arg, "--messages") == 0;
        const bool is_out = std::strcmp(arg, "--out") == 0;
        if (!is_seed && !is_symbols && !is_messages && !is_out) {
            std::fprintf(stderr, "ot_gen: unknown argument '%s' (try --help)\n", arg);
            return kExitUsage;
        }
        if (i + 1 >= argc) {
            std::fprintf(stderr, "ot_gen: %s needs a value\n", arg);
            return kExitUsage;
        }
        const char* value = argv[++i];
        std::uint64_t n = 0;
        if (is_out) {
            out_path = value;
        } else if (is_seed && parse_u64(value, UINT64_MAX, n)) {
            cfg.seed = n;
        } else if (is_symbols && parse_u64(value, UINT16_MAX, n) && n >= 1) {
            cfg.symbols = static_cast<std::uint16_t>(n);
        } else if (is_messages && parse_u64(value, UINT64_MAX, n)) {
            cfg.messages = n;
        } else {
            std::fprintf(stderr, "ot_gen: invalid value '%s' for %s\n", value, arg);
            return kExitUsage;
        }
    }
    if (out_path == nullptr || out_path[0] == '\0') {
        std::fputs("ot_gen: --out FILE is required (try --help)\n", stderr);
        return kExitUsage;
    }

    replay::CaptureWriter writer(out_path);
    if (!writer.ok()) {
        std::fprintf(stderr, "ot_gen: cannot create '%s': %s\n", out_path, std::strerror(errno));
        return 1;
    }

    sim::SyntheticMarket market(cfg);
    Digest digest;
    std::byte buf[itch::kMaxMessageLength];
    std::size_t len = 0;
    Nanos ts = 0;
    while (market.next(buf, len, ts)) {
        if (!writer.write(ts, std::span<const std::byte>(buf, len))) {
            std::fprintf(stderr, "ot_gen: write to '%s' failed after %llu records\n", out_path,
                         static_cast<unsigned long long>(writer.records()));
            return 1;
        }
        digest.update(ts);
        digest.update(buf, len);
    }
    if (!writer.close()) {
        std::fprintf(stderr, "ot_gen: closing '%s' failed (disk full?)\n", out_path);
        return 1;
    }

    std::error_code ec;
    const std::uintmax_t size = std::filesystem::file_size(out_path, ec);
    if (ec) {
        std::fprintf(stderr, "ot_gen: cannot stat '%s': %s\n", out_path, ec.message().c_str());
        return 1;
    }
    std::printf("seed      %llu\n", static_cast<unsigned long long>(cfg.seed));
    std::printf("symbols   %u\n", static_cast<unsigned>(cfg.symbols));
    std::printf("records   %llu\n", static_cast<unsigned long long>(writer.records()));
    std::printf("bytes     %llu\n", static_cast<unsigned long long>(size));
    std::printf("digest    %016llx\n", static_cast<unsigned long long>(digest.value()));
    return 0;
}
