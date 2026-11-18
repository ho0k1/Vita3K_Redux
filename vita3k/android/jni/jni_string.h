#pragma once

#include <jni.h>

#include <string>

// NewStringUTF() takes *modified* UTF-8: 4-byte sequences (emoji) or broken bytes in
// patch titles / notes abort the whole process under CheckJNI. Decode ourselves and
// hand ART UTF-16, replacing anything invalid with U+FFFD.
inline jstring new_jstring_utf8(JNIEnv *env, const std::string &s) {
    std::u16string out;
    out.reserve(s.size());
    const size_t n = s.size();
    size_t i = 0;
    while (i < n) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        char32_t cp = 0xFFFD;
        size_t len = 1;
        if (c < 0x80) {
            cp = c;
        } else if (c >= 0xC2 && c <= 0xDF) {
            len = 2;
        } else if (c >= 0xE0 && c <= 0xEF) {
            len = 3;
        } else if (c >= 0xF0 && c <= 0xF4) {
            len = 4;
        }
        if (len > 1) {
            if (i + len > n) {
                cp = 0xFFFD;
                len = 1;
            } else {
                char32_t v = c & (len == 2 ? 0x1F : len == 3 ? 0x0F : 0x07);
                bool ok = true;
                for (size_t k = 1; k < len; ++k) {
                    const unsigned char cc = static_cast<unsigned char>(s[i + k]);
                    if ((cc & 0xC0) != 0x80) {
                        ok = false;
                        break;
                    }
                    v = (v << 6) | (cc & 0x3F);
                }
                const bool range_ok = (len == 3 && v >= 0x800 && !(v >= 0xD800 && v <= 0xDFFF)) || (len == 4 && v >= 0x10000 && v <= 0x10FFFF) || len == 2;
                if (ok && range_ok) {
                    cp = v;
                } else {
                    cp = 0xFFFD;
                    len = 1;
                }
            }
        }
        if (cp >= 0x10000) {
            cp -= 0x10000;
            out.push_back(static_cast<char16_t>(0xD800 + (cp >> 10)));
            out.push_back(static_cast<char16_t>(0xDC00 + (cp & 0x3FF)));
        } else {
            out.push_back(static_cast<char16_t>(cp));
        }
        i += len;
    }
    return env->NewString(reinterpret_cast<const jchar *>(out.data()), static_cast<jsize>(out.size()));
}
