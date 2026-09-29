// OUCH length-prefixed framing: encode_framed byte layout, buffer limits, and the
// stream decoders under arbitrary chunking, skipped frames and partial tails.

#include <cstdint>
#include <span>
#include <vector>

#include "check.hpp"
#include "optitrade/core/rng.hpp"
#include "optitrade/ouch/codec.hpp"

using namespace optitrade;
using namespace optitrade::ouch;

namespace {

using Bytes = std::vector<std::byte>;

Bytes tight(const Bytes& v) { return Bytes(v.begin(), v.end()); }  // exact-size copy for ASan
std::span<const std::byte> view(const Bytes& v) { return {v.data(), v.size()}; }

void append(Bytes& dst, const Bytes& src) { dst.insert(dst.end(), src.begin(), src.end()); }

Bytes bytes_of(std::initializer_list<int> v) {
    Bytes b;
    for (int x : v) b.push_back(static_cast<std::byte>(x));
    return b;
}

Bytes framed_raw(const Bytes& payload) {
    Bytes f(2 + payload.size());
    f[0] = static_cast<std::byte>(payload.size() >> 8);
    f[1] = static_cast<std::byte>(payload.size());
    for (std::size_t i = 0; i < payload.size(); ++i) f[2 + i] = payload[i];
    return f;
}

template <class M>
Bytes unframed(const M& m) {
    Bytes b(kMaxMessageLength);
    b.resize(encode(m, std::span<std::byte>(b)));
    return b;
}

template <class M>
Bytes framed(const M& m) {
    Bytes b(kMaxMessageLength + 2);
    b.resize(encode_framed(m, std::span<std::byte>(b)));
    return b;
}

// Remembers each delivered message as its unframed wire bytes, in order.
struct Log : NullHandler {
    using NullHandler::on;
    std::vector<Bytes> seen;
    template <class M>
    void keep(const M& m) {
        seen.push_back(unframed(m));
    }
    void on(const EnterOrder& m) { keep(m); }
    void on(const ReplaceOrder& m) { keep(m); }
    void on(const CancelOrder& m) { keep(m); }
    void on(const Accepted& m) { keep(m); }
    void on(const Replaced& m) { keep(m); }
    void on(const Canceled& m) { keep(m); }
    void on(const Executed& m) { keep(m); }
    void on(const Rejected& m) { keep(m); }
};

EnterOrder enter(std::uint64_t id, Qty shares = 100) {
    EnterOrder e;
    e.token = Token::from_id(id);
    e.side = id % 2 ? Side::buy : Side::sell;
    e.short_sell = id % 6 == 0;
    e.shares = shares;
    e.stock = Symbol("ABCD");
    e.price = 1'000'000 + static_cast<Price>(id);
    return e;
}

// A stream mixing valid messages with every kind of frame the decoder must step over.
struct Scenario {
    Bytes stream;
    std::vector<Bytes> expected;  // unframed valid messages, in order
    std::size_t skipped{};
    DecodeStatus last_error{DecodeStatus::ok};
};

Scenario inbound_scenario(std::uint64_t seed, int frames) {
    Scenario sc;
    Rng g(seed);
    for (int i = 0; i < frames; ++i) {
        switch (g.bounded(9)) {
            case 0: {  // zero-length frame
                append(sc.stream, bytes_of({0x00, 0x00}));
                ++sc.skipped;
                sc.last_error = DecodeStatus::truncated;
                break;
            }
            case 1: {  // Modify Order, a real OUCH type this library does not implement
                Bytes payload(28, std::byte{0});
                payload[0] = static_cast<std::byte>('M');
                append(sc.stream, framed_raw(payload));
                ++sc.skipped;
                sc.last_error = DecodeStatus::unknown_type;
                break;
            }
            case 2: {  // an Enter with zero shares
                append(sc.stream, framed(enter(static_cast<std::uint64_t>(i), 0)));
                ++sc.skipped;
                sc.last_error = DecodeStatus::bad_field;
                break;
            }
            case 3: {  // a Cancel with one byte too many
                Bytes payload = unframed(CancelOrder{Token::from_id(5), 1});
                payload.push_back(std::byte{0});
                append(sc.stream, framed_raw(payload));
                ++sc.skipped;
                sc.last_error = DecodeStatus::bad_length;
                break;
            }
            case 4: {  // an Enter cut short but correctly framed
                Bytes payload = unframed(enter(static_cast<std::uint64_t>(i)));
                payload.resize(30);
                append(sc.stream, framed_raw(payload));
                ++sc.skipped;
                sc.last_error = DecodeStatus::truncated;
                break;
            }
            case 5:
            case 6: {
                const CancelOrder c{Token::from_id(g.bounded(1000)), static_cast<Qty>(g.bounded(500))};
                append(sc.stream, framed(c));
                sc.expected.push_back(unframed(c));
                break;
            }
            case 7: {
                ReplaceOrder u;
                u.existing = Token::from_id(g.bounded(1000));
                u.replacement = Token::from_id(1000 + g.bounded(1000));
                u.shares = static_cast<Qty>(1 + g.bounded(999'999));
                u.price = static_cast<Price>(g.bounded(5'000'000));
                append(sc.stream, framed(u));
                sc.expected.push_back(unframed(u));
                break;
            }
            default: {
                const EnterOrder e = enter(g.bounded(1000), static_cast<Qty>(1 + g.bounded(999'999)));
                append(sc.stream, framed(e));
                sc.expected.push_back(unframed(e));
                break;
            }
        }
    }
    return sc;
}

Scenario outbound_scenario(std::uint64_t seed, int frames) {
    Scenario sc;
    Rng g(seed);
    for (int i = 0; i < frames; ++i) {
        const Token t = Token::from_id(g.bounded(100));
        switch (g.bounded(9)) {
            case 0: {  // System Event: valid OUCH, not decoded here
                Bytes payload(10, std::byte{0});
                payload[0] = static_cast<std::byte>('S');
                payload[9] = static_cast<std::byte>('S');
                append(sc.stream, framed_raw(payload));
                ++sc.skipped;
                sc.last_error = DecodeStatus::unknown_type;
                break;
            }
            case 1: {  // Broken Trade
                Bytes payload(32, std::byte{0});
                payload[0] = static_cast<std::byte>('B');
                append(sc.stream, framed_raw(payload));
                ++sc.skipped;
                sc.last_error = DecodeStatus::unknown_type;
                break;
            }
            case 2: {  // Accepted with an impossible order state
                Accepted a;
                a.order_state = '?';
                append(sc.stream, framed(a));
                ++sc.skipped;
                sc.last_error = DecodeStatus::bad_field;
                break;
            }
            case 3: {
                Accepted a;
                a.ts = g.next();
                a.token = t;
                a.shares = static_cast<Qty>(g.bounded(1000));
                a.price = static_cast<Price>(g.bounded(1'000'000));
                append(sc.stream, framed(a));
                sc.expected.push_back(unframed(a));
                break;
            }
            case 4: {
                Replaced p;
                p.a.token = t;
                p.previous = Token::from_id(g.bounded(100));
                p.a.bbo_weight = 'N';
                append(sc.stream, framed(p));
                sc.expected.push_back(unframed(p));
                break;
            }
            case 5: {
                const Canceled c{g.next(), t, static_cast<Qty>(g.bounded(100)), 'U'};
                append(sc.stream, framed(c));
                sc.expected.push_back(unframed(c));
                break;
            }
            case 6:
            case 7: {
                const Executed x{g.next(), t, static_cast<Qty>(1 + g.bounded(100)),
                                 static_cast<Price>(g.bounded(1'000'000)), 'R', g.next()};
                append(sc.stream, framed(x));
                sc.expected.push_back(unframed(x));
                break;
            }
            default: {
                const Rejected j{g.next(), t, 'X'};
                append(sc.stream, framed(j));
                sc.expected.push_back(unframed(j));
                break;
            }
        }
    }
    return sc;
}

// Feeds `stream` to `decode` in the given chunk sizes, keeping the unconsumed tail
// between calls exactly as a socket reader would.
template <class Decode>
StreamResult feed_in_chunks(const Bytes& stream, Rng& g, std::size_t max_chunk, Log& log, Decode decode) {
    StreamResult total;
    Bytes pending;
    std::size_t pos = 0;
    while (pos < stream.size()) {
        const std::size_t n = std::min<std::size_t>(1 + g.bounded(max_chunk), stream.size() - pos);
        pending.insert(pending.end(), stream.begin() + static_cast<std::ptrdiff_t>(pos),
                       stream.begin() + static_cast<std::ptrdiff_t>(pos + n));
        pos += n;
        const Bytes snapshot = tight(pending);
        const StreamResult r = decode(view(snapshot), log);
        total.messages += r.messages;
        total.skipped += r.skipped;
        if (r.skipped != 0) total.last_error = r.last_error;
        pending.erase(pending.begin(), pending.begin() + static_cast<std::ptrdiff_t>(r.consumed));
    }
    total.consumed = stream.size() - pending.size();
    return total;
}

const auto kDecodeIn = [](std::span<const std::byte> b, Log& l) { return decode_inbound_stream(b, l); };
const auto kDecodeOut = [](std::span<const std::byte> b, Log& l) { return decode_outbound_stream(b, l); };

}  // namespace

// ---- encode_framed ------------------------------------------------------------

OT_TEST(framed_cancel_known_answer) {
    const Bytes want = bytes_of({0x00, 0x13,                                            // length 19
                                 'X',                                                   // type
                                 'T', 'O', 'K', 'E', 'N', '0', '0', '0', '0', '0', '0', '0', '0', '1',
                                 0x00, 0x00, 0x00, 0x3C});
    const Bytes got = framed(CancelOrder{Token::from_text("TOKEN000000001"), 60});
    OT_CHECK(got == want);
}

OT_TEST(framed_prefix_is_big_endian_message_length) {
    OT_CHECK_EQ(static_cast<int>(framed(enter(1))[0]), 0x00);
    OT_CHECK_EQ(static_cast<int>(framed(enter(1))[1]), 49);
    OT_CHECK_EQ(static_cast<int>(framed(ReplaceOrder{})[1]), 47);
    OT_CHECK_EQ(static_cast<int>(framed(CancelOrder{})[1]), 19);
    OT_CHECK_EQ(static_cast<int>(framed(Accepted{})[1]), 66);
    OT_CHECK_EQ(static_cast<int>(framed(Replaced{})[1]), 80);
    OT_CHECK_EQ(static_cast<int>(framed(Canceled{})[1]), 28);
    OT_CHECK_EQ(static_cast<int>(framed(Executed{})[1]), 40);
    OT_CHECK_EQ(static_cast<int>(framed(Rejected{})[1]), 24);
    OT_CHECK_EQ(framed(Replaced{}).size(), std::size_t{82});
    // The framed bytes after the prefix are exactly the unframed message.
    const Bytes f = framed(enter(9));
    OT_CHECK(Bytes(f.begin() + 2, f.end()) == unframed(enter(9)));
}

namespace {

template <class M>
void check_framed_bounds(const M& m, std::size_t message_length) {
    const std::size_t total = message_length + 2;
    for (std::size_t n = 0; n < total; ++n) {
        Bytes buf(n, std::byte{0xEE});
        OT_CHECK_EQ(encode_framed(m, std::span<std::byte>(buf)), std::size_t{0});
        for (std::byte b : buf) OT_CHECK(b == std::byte{0xEE});
    }
    Bytes exact(total);
    OT_CHECK_EQ(encode_framed(m, std::span<std::byte>(exact)), total);
    Bytes big(total + 3, std::byte{0xEE});
    OT_CHECK_EQ(encode_framed(m, std::span<std::byte>(big)), total);
    for (std::size_t i = total; i < big.size(); ++i) OT_CHECK(big[i] == std::byte{0xEE});
}

}  // namespace

OT_TEST(framed_encoders_respect_the_buffer) {
    check_framed_bounds(enter(1), 49);
    check_framed_bounds(ReplaceOrder{}, 47);
    check_framed_bounds(CancelOrder{}, 19);
    check_framed_bounds(Accepted{}, 66);
    check_framed_bounds(Replaced{}, 80);
    check_framed_bounds(Canceled{}, 28);
    check_framed_bounds(Executed{}, 40);
    check_framed_bounds(Rejected{}, 24);
}

OT_TEST(framed_encoder_writes_nothing_for_an_unrepresentable_price) {
    EnterOrder e = enter(1);
    e.price = -5;
    Bytes buf(100, std::byte{0xEE});
    OT_CHECK_EQ(encode_framed(e, std::span<std::byte>(buf)), std::size_t{0});
    for (std::byte b : buf) OT_CHECK(b == std::byte{0xEE});  // not even the length prefix
}

// ---- stream decoding ----------------------------------------------------------

OT_TEST(stream_of_valid_frames_delivers_every_message_in_order) {
    Bytes stream;
    append(stream, framed(enter(1)));
    append(stream, framed(CancelOrder{Token::from_id(1), 0}));
    append(stream, framed(enter(2)));
    Log log;
    const Bytes s = tight(stream);
    const StreamResult r = decode_inbound_stream(view(s), log);
    OT_CHECK_EQ(r.consumed, s.size());
    OT_CHECK_EQ(r.messages, std::size_t{3});
    OT_CHECK_EQ(r.skipped, std::size_t{0});
    OT_CHECK_EQ(r.last_error, DecodeStatus::ok);
    OT_CHECK_EQ(log.seen.size(), std::size_t{3});
    OT_CHECK(log.seen[0] == unframed(enter(1)));
    OT_CHECK(log.seen[1] == unframed(CancelOrder{Token::from_id(1), 0}));
    OT_CHECK(log.seen[2] == unframed(enter(2)));
}

OT_TEST(empty_and_one_byte_buffers_consume_nothing) {
    Log log;
    const StreamResult a = decode_inbound_stream(std::span<const std::byte>{}, log);
    OT_CHECK_EQ(a.consumed, std::size_t{0});
    OT_CHECK_EQ(a.messages + a.skipped, std::size_t{0});
    const Bytes one = bytes_of({0x00});
    const StreamResult b = decode_outbound_stream(view(one), log);
    OT_CHECK_EQ(b.consumed, std::size_t{0});
    OT_CHECK_EQ(b.messages + b.skipped, std::size_t{0});
}

OT_TEST(skipped_frames_are_counted_and_decoding_continues) {
    const Scenario sc = inbound_scenario(11, 400);
    OT_CHECK(sc.skipped > 20);
    OT_CHECK(sc.expected.size() > 100);
    Log log;
    const Bytes s = tight(sc.stream);
    const StreamResult r = decode_inbound_stream(view(s), log);
    OT_CHECK_EQ(r.consumed, s.size());
    OT_CHECK_EQ(r.messages, sc.expected.size());
    OT_CHECK_EQ(r.skipped, sc.skipped);
    OT_CHECK_EQ(r.last_error, sc.last_error);
    OT_CHECK(log.seen == sc.expected);
}

OT_TEST(each_kind_of_bad_frame_reports_its_own_status) {
    const struct { Bytes frame; DecodeStatus status; } cases[] = {
        {bytes_of({0x00, 0x00}), DecodeStatus::truncated},
        {framed_raw(bytes_of({'M', 0, 0, 0})), DecodeStatus::unknown_type},
        {framed_raw(bytes_of({'X', 0, 0})), DecodeStatus::truncated},
        {framed_raw(Bytes(20, static_cast<std::byte>('X'))), DecodeStatus::bad_length},
        {framed(enter(4, 0)), DecodeStatus::bad_field},
        {framed(enter(4, 1'000'000)), DecodeStatus::bad_field},
    };
    for (const auto& c : cases) {
        Bytes stream = c.frame;
        append(stream, framed(enter(8)));  // a good frame behind it must still arrive
        Log log;
        const Bytes s = tight(stream);
        const StreamResult r = decode_inbound_stream(view(s), log);
        OT_CHECK_EQ(r.last_error, c.status);
        OT_CHECK_EQ(r.skipped, std::size_t{1});
        OT_CHECK_EQ(r.messages, std::size_t{1});
        OT_CHECK_EQ(r.consumed, s.size());
    }
}

OT_TEST(partial_trailing_frame_is_left_unconsumed_at_every_cut) {
    const Bytes head = [] {
        Bytes b;
        append(b, framed(enter(1)));
        append(b, framed(CancelOrder{Token::from_id(1), 5}));
        return b;
    }();
    const Bytes tail = framed(enter(2));
    for (std::size_t cut = 0; cut < tail.size(); ++cut) {
        Bytes stream = head;
        stream.insert(stream.end(), tail.begin(), tail.begin() + static_cast<std::ptrdiff_t>(cut));
        Log log;
        const Bytes s = tight(stream);
        const StreamResult r = decode_inbound_stream(view(s), log);
        OT_CHECK_EQ(r.consumed, head.size());  // points at the first byte of the partial frame
        OT_CHECK_EQ(r.messages, std::size_t{2});
        OT_CHECK_EQ(r.skipped, std::size_t{0});
        OT_CHECK_EQ(log.seen.size(), std::size_t{2});
    }
    Bytes whole = head;
    append(whole, tail);
    Log log;
    const Bytes s = tight(whole);
    OT_CHECK_EQ(decode_inbound_stream(view(s), log).consumed, whole.size());
}

OT_TEST(a_huge_declared_length_waits_for_the_bytes_and_then_skips_the_frame) {
    Bytes stream = bytes_of({0xFF, 0xFF, 'O', 0, 0});  // claims 65535 bytes, has 3
    Log log;
    Bytes s = tight(stream);
    StreamResult r = decode_inbound_stream(view(s), log);
    OT_CHECK_EQ(r.consumed, std::size_t{0});
    OT_CHECK_EQ(r.messages + r.skipped, std::size_t{0});

    stream.resize(2 + 65535, std::byte{0});  // now complete, still far too long for an Enter
    s = tight(stream);
    r = decode_inbound_stream(view(s), log);
    OT_CHECK_EQ(r.consumed, s.size());
    OT_CHECK_EQ(r.skipped, std::size_t{1});
    OT_CHECK_EQ(r.last_error, DecodeStatus::bad_length);
    OT_CHECK_EQ(log.seen.size(), std::size_t{0});
}

OT_TEST(chunked_inbound_stream_matches_one_shot_decode) {
    const Scenario sc = inbound_scenario(21, 600);
    for (std::size_t max_chunk : {std::size_t{1}, std::size_t{2}, std::size_t{7}, std::size_t{50}, std::size_t{400},
                                  std::size_t{100000}}) {
        Rng g(max_chunk);
        Log log;
        const StreamResult r = feed_in_chunks(sc.stream, g, max_chunk, log, kDecodeIn);
        OT_CHECK_EQ(r.consumed, sc.stream.size());
        OT_CHECK_EQ(r.messages, sc.expected.size());
        OT_CHECK_EQ(r.skipped, sc.skipped);
        OT_CHECK_EQ(r.last_error, sc.last_error);
        OT_CHECK(log.seen == sc.expected);
    }
}

OT_TEST(chunked_outbound_stream_matches_one_shot_decode) {
    const Scenario sc = outbound_scenario(31, 600);
    OT_CHECK(sc.skipped > 20);
    OT_CHECK(sc.expected.size() > 100);
    {
        Log log;
        const Bytes s = tight(sc.stream);
        const StreamResult r = decode_outbound_stream(view(s), log);
        OT_CHECK_EQ(r.consumed, s.size());
        OT_CHECK_EQ(r.messages, sc.expected.size());
        OT_CHECK_EQ(r.skipped, sc.skipped);
        OT_CHECK_EQ(r.last_error, sc.last_error);
        OT_CHECK(log.seen == sc.expected);
    }
    for (std::size_t max_chunk : {std::size_t{1}, std::size_t{3}, std::size_t{33}, std::size_t{1000}}) {
        Rng g(max_chunk + 7);
        Log log;
        const StreamResult r = feed_in_chunks(sc.stream, g, max_chunk, log, kDecodeOut);
        OT_CHECK_EQ(r.consumed, sc.stream.size());
        OT_CHECK_EQ(r.messages, sc.expected.size());
        OT_CHECK_EQ(r.skipped, sc.skipped);
        OT_CHECK(log.seen == sc.expected);
    }
}

OT_TEST(every_two_way_split_gives_the_same_result) {
    const Scenario sc = inbound_scenario(41, 12);
    for (std::size_t split = 0; split <= sc.stream.size(); ++split) {
        Log log;
        Bytes pending(sc.stream.begin(), sc.stream.begin() + static_cast<std::ptrdiff_t>(split));
        Bytes snap = tight(pending);
        StreamResult a = decode_inbound_stream(view(snap), log);
        pending.erase(pending.begin(), pending.begin() + static_cast<std::ptrdiff_t>(a.consumed));
        pending.insert(pending.end(), sc.stream.begin() + static_cast<std::ptrdiff_t>(split), sc.stream.end());
        snap = tight(pending);
        const StreamResult b = decode_inbound_stream(view(snap), log);
        OT_CHECK_EQ(a.consumed + b.consumed, sc.stream.size());
        OT_CHECK_EQ(a.messages + b.messages, sc.expected.size());
        OT_CHECK_EQ(a.skipped + b.skipped, sc.skipped);
        OT_CHECK(log.seen == sc.expected);
    }
}

OT_TEST(directions_do_not_mix_in_a_stream) {
    // A framed Accepted fed to the inbound decoder is skipped as an unknown type,
    // and the same bytes are delivered by the outbound decoder.
    Bytes stream;
    append(stream, framed(Accepted{}));
    append(stream, framed(enter(3)));
    const Bytes s = tight(stream);
    Log inbound;
    const StreamResult ri = decode_inbound_stream(view(s), inbound);
    OT_CHECK_EQ(ri.messages, std::size_t{1});
    OT_CHECK_EQ(ri.skipped, std::size_t{1});
    OT_CHECK_EQ(ri.last_error, DecodeStatus::unknown_type);
    Log outbound;
    const StreamResult ro = decode_outbound_stream(view(s), outbound);
    OT_CHECK_EQ(ro.messages, std::size_t{1});
    OT_CHECK_EQ(ro.skipped, std::size_t{1});
    OT_CHECK_EQ(outbound.seen.size(), std::size_t{1});
    OT_CHECK(outbound.seen[0] == unframed(Accepted{}));
}

OT_TEST(random_garbage_never_reads_out_of_bounds_and_accounts_for_every_byte) {
    Rng g(77);
    for (int i = 0; i < 3000; ++i) {
        Bytes junk(g.bounded(300));
        for (std::byte& b : junk) b = static_cast<std::byte>(g.bounded(256));
        const Bytes s = tight(junk);
        Log a, b;
        const StreamResult ri = decode_inbound_stream(view(s), a);
        const StreamResult ro = decode_outbound_stream(view(s), b);
        for (const StreamResult& r : {ri, ro}) {
            OT_CHECK(r.consumed <= s.size());
            // The unconsumed tail is strictly shorter than the frame it starts.
            const std::size_t rest = s.size() - r.consumed;
            if (rest >= 2) {
                const std::size_t declared = (static_cast<std::size_t>(s[r.consumed]) << 8) |
                                             static_cast<std::size_t>(s[r.consumed + 1]);
                OT_CHECK(rest - 2 < declared);
            }
        }
        OT_CHECK_EQ(a.seen.size(), ri.messages);
        OT_CHECK_EQ(b.seen.size(), ro.messages);
    }
}

OT_TEST_MAIN()
