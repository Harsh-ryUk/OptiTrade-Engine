#pragma once

#include <cstdint>

namespace optitrade {

// Result of decoding one wire message (ITCH, OUCH, MoldUDP64).
enum class DecodeStatus : std::uint8_t {
    ok = 0,
    truncated,     // fewer bytes than the message type requires
    unknown_type,  // message type byte not supported by this library
    bad_length,    // more bytes than the message type allows (or bad framing)
    bad_field,     // a field holds a value the protocol forbids
};

constexpr const char* to_string(DecodeStatus s) noexcept {
    switch (s) {
        case DecodeStatus::ok: return "ok";
        case DecodeStatus::truncated: return "truncated";
        case DecodeStatus::unknown_type: return "unknown_type";
        case DecodeStatus::bad_length: return "bad_length";
        case DecodeStatus::bad_field: return "bad_field";
    }
    return "?";
}

}  // namespace optitrade
