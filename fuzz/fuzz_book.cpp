// libFuzzer entry point for the order books. Build with -DOPTITRADE_BUILD_FUZZERS=ON
// (clang only); the same body runs deterministically in tests/test_book_fuzz_smoke.cpp.

#include <cstddef>
#include <cstdint>

#include "book_fuzz_body.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    optitrade::fuzz::book_one(data, size);
    return 0;
}
