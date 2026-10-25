// json.h: a minimal JSON, just what the protocol with wa-core needs (one line per
// object). Reads into a Json tree and writes with json_quote(). Texts are UTF-8.
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

struct Json {
    enum Type : uint8_t { Null, Bool, Num, Str, Arr, Obj };
    Type type = Null;
    bool b = false;
    double n = 0;
    std::string s;
    std::vector<Json> a;
    std::vector<std::pair<std::string, Json>> o;

    static const Json &none() {
        static const Json j;
        return j;
    }
    const Json &operator[](const char *key) const {
        if (type == Obj)
            for (const auto &kv : o)
                if (kv.first == key) return kv.second;
        return none();
    }
    const Json &operator[](size_t i) const { return type == Arr && i < a.size() ? a[i] : none(); }
    size_t size() const { return type == Arr ? a.size() : type == Obj ? o.size() : 0; }
    bool has(const char *key) const { return &(*this)[key] != &none(); }
    std::string str(const char *def = "") const { return type == Str ? s : def; }
    double num(double def = 0) const { return type == Num ? n : type == Bool ? (b ? 1 : 0) : def; }
    int64_t i64(int64_t def = 0) const { return type == Num ? (int64_t)n : def; }
    bool boolean(bool def = false) const { return type == Bool ? b : type == Num ? n != 0 : def; }

    // false if not valid JSON (what is left in out is not valid)
    static bool parse(const char *p, size_t len, Json &out) {
        Parser ps{ p, p + len, 0 };
        ps.ws();
        if (!ps.value(out)) return false;
        ps.ws();
        return ps.p == ps.end;
    }

private:
    struct Parser {
        const char *p, *end;
        int depth;
        void ws() {
            while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
        }
        bool lit(const char *w) {
            size_t k = strlen(w);
            if ((size_t)(end - p) < k || memcmp(p, w, k) != 0) return false;
            p += k;
            return true;
        }
        static void put_utf8(std::string &s, uint32_t c) {
            if (c < 0x80) s += (char)c;
            else if (c < 0x800) {
                s += (char)(0xC0 | (c >> 6));
                s += (char)(0x80 | (c & 63));
            } else if (c < 0x10000) {
                s += (char)(0xE0 | (c >> 12));
                s += (char)(0x80 | ((c >> 6) & 63));
                s += (char)(0x80 | (c & 63));
            } else {
                s += (char)(0xF0 | (c >> 18));
                s += (char)(0x80 | ((c >> 12) & 63));
                s += (char)(0x80 | ((c >> 6) & 63));
                s += (char)(0x80 | (c & 63));
            }
        }
        bool hex4(uint32_t &v) {
            if (end - p < 4) return false;
            v = 0;
            for (int i = 0; i < 4; i++) {
                char c = *p++;
                v <<= 4;
                if (c >= '0' && c <= '9') v |= c - '0';
                else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
                else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
                else return false;
            }
            return true;
        }
        bool string(std::string &s) {
            if (p >= end || *p != '"') return false;
            p++;
            for (;;) {
                const char *run = p;
                while (p < end && *p != '"' && *p != '\\') p++;
                s.append(run, p - run);
                if (p >= end) return false;
                if (*p == '"') {
                    p++;
                    return true;
                }
                p++; // the backslash
                if (p >= end) return false;
                char e = *p++;
                switch (e) {
                case '"': s += '"'; break;
                case '\\': s += '\\'; break;
                case '/': s += '/'; break;
                case 'b': s += '\b'; break;
                case 'f': s += '\f'; break;
                case 'n': s += '\n'; break;
                case 'r': s += '\r'; break;
                case 't': s += '\t'; break;
                case 'u': {
                    uint32_t c;
                    if (!hex4(c)) return false;
                    if (c >= 0xD800 && c < 0xDC00 && end - p >= 6 && p[0] == '\\' && p[1] == 'u') {
                        p += 2;
                        uint32_t lo;
                        if (!hex4(lo)) return false;
                        if (lo >= 0xDC00 && lo < 0xE000) c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00);
                        else c = 0xFFFD;
                    } else if (c >= 0xD800 && c < 0xE000) {
                        c = 0xFFFD;
                    }
                    put_utf8(s, c);
                    break;
                }
                default: return false;
                }
            }
        }
        bool value(Json &v) {
            if (p >= end || ++depth > 64) return false;
            bool ok = false;
            char c = *p;
            if (c == '{') {
                v.type = Obj;
                p++;
                ws();
                if (p < end && *p == '}') {
                    p++;
                    ok = true;
                } else {
                    for (;;) {
                        ws();
                        std::pair<std::string, Json> kv;
                        if (!string(kv.first)) break;
                        ws();
                        if (p >= end || *p != ':') break;
                        p++;
                        ws();
                        if (!value(kv.second)) break;
                        v.o.push_back(std::move(kv));
                        ws();
                        if (p < end && *p == ',') {
                            p++;
                            continue;
                        }
                        if (p < end && *p == '}') {
                            p++;
                            ok = true;
                        }
                        break;
                    }
                }
            } else if (c == '[') {
                v.type = Arr;
                p++;
                ws();
                if (p < end && *p == ']') {
                    p++;
                    ok = true;
                } else {
                    for (;;) {
                        ws();
                        v.a.emplace_back();
                        if (!value(v.a.back())) break;
                        ws();
                        if (p < end && *p == ',') {
                            p++;
                            continue;
                        }
                        if (p < end && *p == ']') {
                            p++;
                            ok = true;
                        }
                        break;
                    }
                }
            } else if (c == '"') {
                v.type = Str;
                ok = string(v.s);
            } else if (c == 't') {
                v.type = Bool;
                v.b = true;
                ok = lit("true");
            } else if (c == 'f') {
                v.type = Bool;
                ok = lit("false");
            } else if (c == 'n') {
                ok = lit("null");
            } else if (c == '-' || (c >= '0' && c <= '9')) {
                char buf[64];
                size_t k = 0;
                while (p < end && k < sizeof(buf) - 1 && (strchr("+-.eE", *p) || (*p >= '0' && *p <= '9'))) buf[k++] = *p++;
                buf[k] = 0;
                char *stop = nullptr;
                v.type = Num;
                v.n = strtod(buf, &stop);
                ok = stop == buf + k && k > 0;
            }
            depth--;
            return ok;
        }
    };
};

// A UTF-8 text as a JSON string, with its quotes.
inline std::string json_quote(const std::string &s) {
    std::string r;
    r.reserve(s.size() + 2);
    r += '"';
    for (unsigned char c : s) {
        switch (c) {
        case '"': r += "\\\""; break;
        case '\\': r += "\\\\"; break;
        case '\n': r += "\\n"; break;
        case '\r': r += "\\r"; break;
        case '\t': r += "\\t"; break;
        default:
            if (c < 0x20) {
                char u[8];
                snprintf(u, sizeof(u), "\\u%04x", c);
                r += u;
            } else {
                r += (char)c;
            }
        }
    }
    r += '"';
    return r;
}

// standard base64 (the one for the QR bits); anything outside the alphabet is skipped
inline std::vector<uint8_t> base64_decode(const std::string &in) {
    std::vector<uint8_t> out;
    out.reserve(in.size() * 3 / 4);
    uint32_t acc = 0;
    int bits = 0;
    for (char ch : in) {
        int v;
        if (ch >= 'A' && ch <= 'Z') v = ch - 'A';
        else if (ch >= 'a' && ch <= 'z') v = ch - 'a' + 26;
        else if (ch >= '0' && ch <= '9') v = ch - '0' + 52;
        else if (ch == '+' || ch == '-') v = 62;
        else if (ch == '/' || ch == '_') v = 63;
        else continue;
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back((uint8_t)((acc >> bits) & 0xFF));
        }
    }
    return out;
}
