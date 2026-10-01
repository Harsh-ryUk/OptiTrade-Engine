// libFuzzer entry point for the OUCH codec. The invariants live in
// ouch_fuzz_body.hpp so the same checks run in the deterministic smoke test.

#include <cstddef>
#include <cstdint>

#include "ouch_fuzz_body.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    optitrade::fuzz::ouch_one(data, size);
    return 0;
}
