// libFuzzer entry point for the ITCH codec. Build with -DOPTITRADE_BUILD_FUZZERS=ON
// (clang only); the same body runs deterministically in tests/test_itch_fuzz_smoke.cpp.

#include <cstddef>
#include <cstdint>

#include "itch_fuzz_body.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    optitrade::fuzz::itch_one(data, size);
    return 0;
}
