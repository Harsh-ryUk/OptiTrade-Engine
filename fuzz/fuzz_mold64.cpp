// libFuzzer entry point for the MoldUDP64 codec. The checks live in mold64_fuzz_body.hpp so
// the same code runs under tests/test_mold64_fuzz_smoke.cpp on toolchains without libFuzzer.

#include <cstddef>
#include <cstdint>

#include "mold64_fuzz_body.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    optitrade::fuzz::mold64_one(data, size);
    return 0;
}
