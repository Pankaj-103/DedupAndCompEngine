#include "hash.h"

#include <fstream>
#include <stdexcept>
#include <vector>

#include "blake3.h"

std::string to_hex(const Hash256& h) {
    static const char digits[] = "0123456789abcdef";
    std::string s(64, '0');
    for (size_t i = 0; i < h.size(); ++i) {
        s[2 * i] = digits[h[i] >> 4];
        s[2 * i + 1] = digits[h[i] & 0xf];
    }
    return s;
}

Hash256 hash_file(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open file: " + p.string());

    blake3_hasher hasher;
    blake3_hasher_init(&hasher);

    std::vector<char> buf(1 << 20);
    while (in.read(buf.data(), static_cast<std::streamsize>(buf.size())) || in.gcount() > 0) {
        blake3_hasher_update(&hasher, buf.data(), static_cast<size_t>(in.gcount()));
    }
    if (in.bad()) throw std::runtime_error("read error: " + p.string());

    Hash256 out;
    blake3_hasher_finalize(&hasher, out.data(), out.size());
    return out;
}
