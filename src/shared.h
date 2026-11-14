#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <string>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace gitmo_shared {

inline bool GetEnvironmentVariable(const char* name, std::string& value) {
    char* buffer = nullptr;
    std::size_t size = 0;
    errno_t error = _dupenv_s(&buffer, &size, name);
    if (error != 0) {
        std::free(buffer);
        value.clear();
        return false;
    }
    bool found = buffer != nullptr;
    value = found ? buffer : "";
    std::free(buffer);
    return found;
}

inline bool IsEnvironmentVariableSet(const char* name) {
    char* buffer = nullptr;
    std::size_t size = 0;
    errno_t error = _dupenv_s(&buffer, &size, name);
    bool found = error == 0 && buffer != nullptr;
    std::free(buffer);
    return found;
}

inline std::string TrimAsciiWhitespace(const std::string& value) {
    std::size_t first = value.find_first_not_of(" \t\r\n");
    std::size_t last = value.find_last_not_of(" \t\r\n");
    return first == std::string::npos ? std::string() : value.substr(first, last - first + 1);
}

namespace detail {

inline bool ReadJsonHex4(const char*& cursor, const char* end, uint32_t& value) {
    if (end - cursor < 4) return false;
    uint32_t parsed = 0;
    for (int i = 0; i < 4; i++) {
        unsigned char c = (unsigned char)cursor[i];
        uint32_t digit = c >= '0' && c <= '9' ? c - '0' :
                         c >= 'a' && c <= 'f' ? c - 'a' + 10 :
                         c >= 'A' && c <= 'F' ? c - 'A' + 10 : 16;
        if (digit == 16) return false;
        parsed = (parsed << 4) | digit;
    }
    cursor += 4;
    value = parsed;
    return true;
}

inline void AppendUtf8(std::string& out, uint32_t cp) {
    if (cp <= 0x7F) out.push_back((char)cp);
    else if (cp <= 0x7FF) {
        out.push_back((char)(0xC0 | (cp >> 6)));
        out.push_back((char)(0x80 | (cp & 0x3F)));
    } else if (cp <= 0xFFFF) {
        out.push_back((char)(0xE0 | (cp >> 12)));
        out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back((char)(0x80 | (cp & 0x3F)));
    } else {
        out.push_back((char)(0xF0 | (cp >> 18)));
        out.push_back((char)(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back((char)(0x80 | (cp & 0x3F)));
    }
}

}  // namespace detail

inline bool ReadJsonString(const char*& cursor, const char* end, std::string& out) {
    if (cursor >= end || *cursor++ != '"') return false;
    out.clear();
    while (cursor < end) {
        unsigned char c = (unsigned char)*cursor++;
        if (c == '"') return true;
        if (c < 0x20) return false;
        if (c != '\\') {
            out.push_back((char)c);
            continue;
        }
        if (cursor >= end) return false;
        char escaped = *cursor++;
        switch (escaped) {
        case '"': case '\\': case '/': out.push_back(escaped); break;
        case 'b': out.push_back('\b'); break;
        case 'f': out.push_back('\f'); break;
        case 'n': out.push_back('\n'); break;
        case 'r': out.push_back('\r'); break;
        case 't': out.push_back('\t'); break;
        case 'u': {
            uint32_t cp;
            if (!detail::ReadJsonHex4(cursor, end, cp)) return false;
            if (cp >= 0xD800 && cp <= 0xDBFF) {
                if (end - cursor < 6 || cursor[0] != '\\' || cursor[1] != 'u') return false;
                cursor += 2;
                uint32_t low;
                if (!detail::ReadJsonHex4(cursor, end, low) || low < 0xDC00 || low > 0xDFFF) return false;
                cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
            } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                return false;
            }
            detail::AppendUtf8(out, cp);
            break;
        }
        default: return false;
        }
    }
    return false;
}

inline bool ReadJsonString(const std::string& json, std::size_t& pos, std::string& out) {
    if (pos > json.size()) return false;
    const char* begin = json.c_str();
    const char* cursor = begin + pos;
    bool ok = ReadJsonString(cursor, begin + json.size(), out);
    pos = (size_t)(cursor - begin);
    return ok;
}

inline bool SafeCopyMemory(void* dst, const void* src, size_t size) {
    __try { std::memcpy(dst, src, size); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

}  // namespace gitmo_shared
