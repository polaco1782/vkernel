#pragma once

#include "kobj.h"

namespace vk::kobj::detail {

inline auto cstrlen(const char* s) -> usize {
    if (s == null) return 0;
    usize n = 0;
    while (s[n] != '\0') ++n;
    return n;
}

inline auto append_ch(char* out, usize out_cap, usize pos, char ch) -> usize {
    if (out_cap == 0) return 0;
    if (pos + 1 < out_cap) {
        out[pos++] = ch;
        out[pos] = '\0';
    }
    return pos;
}

inline auto append_str(char* out, usize out_cap, usize pos, const char* s) -> usize {
    if (s == null) return pos;
    for (usize i = 0; s[i] != '\0'; ++i) {
        pos = append_ch(out, out_cap, pos, s[i]);
    }
    return pos;
}

inline auto render_u64(char* out, usize cap, usize pos, u64 v) -> usize {
    char tmp[21];
    usize n = 0;
    if (v == 0) {
        return append_ch(out, cap, pos, '0');
    }
    while (v > 0 && n < sizeof(tmp)) {
        tmp[n++] = static_cast<char>('0' + (v % 10));
        v /= 10;
    }
    while (n > 0) {
        pos = append_ch(out, cap, pos, tmp[--n]);
    }
    return pos;
}

inline auto render_i64(char* out, usize cap, usize pos, i64 v) -> usize {
    if (v < 0) {
        pos = append_ch(out, cap, pos, '-');
        const u64 uv = static_cast<u64>(-(v + 1)) + 1;
        return render_u64(out, cap, pos, uv);
    }
    return render_u64(out, cap, pos, static_cast<u64>(v));
}

inline auto tag_name(KTag tag) -> const char* {
    switch (tag) {
        case KTag::U64: return "u64";
        case KTag::I64: return "i64";
        case KTag::Bool: return "bool";
        case KTag::Str: return "str";
        case KTag::Enum: return "enum";
        case KTag::Struct: return "struct";
        case KTag::Stream: return "stream";
        case KTag::Err: return "err";
        default: return "unknown";
    }
}

inline auto parse_u64_segment(const char* s, usize n, u64* out) -> bool {
    if (s == null || out == null || n == 0) return false;
    u64 value = 0;
    for (usize i = 0; i < n; ++i) {
        if (s[i] < '0' || s[i] > '9') return false;
        value = (value * 10ULL) + static_cast<u64>(s[i] - '0');
    }
    *out = value;
    return true;
}

} // namespace vk::kobj::detail
