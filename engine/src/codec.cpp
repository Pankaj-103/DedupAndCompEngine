#include "codec.h"

#include <memory>
#include <stdexcept>
#include <vector>

#include <zstd.h>

namespace {

void check_zstd(size_t code, const char* what) {
    if (ZSTD_isError(code))
        throw std::runtime_error(std::string(what) + ": " + ZSTD_getErrorName(code));
}

std::ifstream open_input(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open file: " + p.string());
    return in;
}

}  // namespace

uint64_t zstd_compress_file(const std::filesystem::path& src,
                            const std::filesystem::path& dst, int level) {
    std::ifstream in = open_input(src);
    std::ofstream out(dst, std::ios::binary | std::ios::trunc);
    if (!out) throw std::runtime_error("cannot create file: " + dst.string());

    std::unique_ptr<ZSTD_CCtx, size_t (*)(ZSTD_CCtx*)> cctx(ZSTD_createCCtx(), ZSTD_freeCCtx);
    if (!cctx) throw std::runtime_error("ZSTD_createCCtx failed");
    check_zstd(ZSTD_CCtx_setParameter(cctx.get(), ZSTD_c_compressionLevel, level),
               "set compression level");

    std::vector<char> inBuf(ZSTD_CStreamInSize());
    std::vector<char> outBuf(ZSTD_CStreamOutSize());
    uint64_t written = 0;

    bool eof = false;
    while (!eof) {
        in.read(inBuf.data(), static_cast<std::streamsize>(inBuf.size()));
        if (in.bad()) throw std::runtime_error("read error: " + src.string());
        const size_t readBytes = static_cast<size_t>(in.gcount());
        eof = in.eof();

        ZSTD_inBuffer input{inBuf.data(), readBytes, 0};
        const ZSTD_EndDirective mode = eof ? ZSTD_e_end : ZSTD_e_continue;
        size_t remaining = 0;
        do {
            ZSTD_outBuffer output{outBuf.data(), outBuf.size(), 0};
            remaining = ZSTD_compressStream2(cctx.get(), &output, &input, mode);
            check_zstd(remaining, "zstd compress");
            out.write(outBuf.data(), static_cast<std::streamsize>(output.pos));
            written += output.pos;
        } while (eof ? (remaining != 0) : (input.pos < input.size));
    }

    out.flush();
    if (!out) throw std::runtime_error("write error: " + dst.string());
    return written;
}

void zstd_decompress_file(const std::filesystem::path& src, std::ofstream& out) {
    std::ifstream in = open_input(src);

    std::unique_ptr<ZSTD_DCtx, size_t (*)(ZSTD_DCtx*)> dctx(ZSTD_createDCtx(), ZSTD_freeDCtx);
    if (!dctx) throw std::runtime_error("ZSTD_createDCtx failed");

    std::vector<char> inBuf(ZSTD_DStreamInSize());
    std::vector<char> outBuf(ZSTD_DStreamOutSize());

    size_t lastRet = 0;
    for (;;) {
        in.read(inBuf.data(), static_cast<std::streamsize>(inBuf.size()));
        if (in.bad()) throw std::runtime_error("read error: " + src.string());
        const size_t readBytes = static_cast<size_t>(in.gcount());
        if (readBytes == 0) break;

        ZSTD_inBuffer input{inBuf.data(), readBytes, 0};
        while (input.pos < input.size) {
            ZSTD_outBuffer output{outBuf.data(), outBuf.size(), 0};
            lastRet = ZSTD_decompressStream(dctx.get(), &output, &input);
            check_zstd(lastRet, "zstd decompress");
            out.write(outBuf.data(), static_cast<std::streamsize>(output.pos));
        }
        if (in.eof()) break;
    }
    if (lastRet != 0)
        throw std::runtime_error("truncated zstd stream: " + src.string());
    if (!out) throw std::runtime_error("write error while restoring");
}

void append_raw_file(const std::filesystem::path& src, std::ofstream& out) {
    std::ifstream in = open_input(src);
    std::vector<char> buf(1 << 20);
    while (in.read(buf.data(), static_cast<std::streamsize>(buf.size())) || in.gcount() > 0) {
        out.write(buf.data(), in.gcount());
    }
    if (in.bad()) throw std::runtime_error("read error: " + src.string());
    if (!out) throw std::runtime_error("write error while restoring");
}
