#include "fsutil.h"

#include <blake3.h>

#include <algorithm>
#include <cstring>

#if defined(__unix__) || defined(__APPLE__)
#  include <unistd.h>
#endif

namespace graft {

bool File::open(const fs::path &p, const char *mode) {
    close();
    f_ = std::fopen(p.string().c_str(), mode);
    return f_ != nullptr;
}

bool File::close() {
    if (!f_) return true;
    const bool ok = std::fclose(f_) == 0;
    f_ = nullptr;
    return ok;
}

bool file_seek(std::FILE *f, uint64_t off) {
#if defined(_WIN32)
    return _fseeki64(f, static_cast<long long>(off), SEEK_SET) == 0;
#else
    return fseeko(f, static_cast<off_t>(off), SEEK_SET) == 0;
#endif
}

graft_status read_whole(const fs::path &p, std::vector<uint8_t> &out) {
    File f;
    if (!f.open(p, "rb")) return GRAFT_E_IO;
    out.clear();
    uint8_t buf[1 << 16];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f.get())) > 0) out.insert(out.end(), buf, buf + n);
    return std::ferror(f.get()) ? GRAFT_E_IO : GRAFT_OK;
}

namespace {

// Zeroes the parts of `buf` (holding bytes [off, off + n)) that fall in a region.
void zero_regions(uint8_t *buf, uint64_t off, size_t n, const Regions &zeroed) {
    for (const auto &[ro, rl] : zeroed) {
        const uint64_t lo = std::max(ro, off), hi = std::min(ro + rl, off + n);
        if (lo < hi) std::memset(buf + (lo - off), 0, static_cast<size_t>(hi - lo));
    }
}

}  // namespace

graft_status hash_file(const fs::path &p, const Regions &zeroed, Hash &out, uint64_t *size) {
    File f;
    if (!f.open(p, "rb")) return GRAFT_E_IO;
    blake3_hasher h;
    blake3_hasher_init(&h);
    std::vector<uint8_t> buf(1 << 16);
    uint64_t off = 0;
    size_t n;
    while ((n = std::fread(buf.data(), 1, buf.size(), f.get())) > 0) {
        zero_regions(buf.data(), off, n, zeroed);
        blake3_hasher_update(&h, buf.data(), n);
        off += n;
    }
    if (std::ferror(f.get())) return GRAFT_E_IO;
    blake3_hasher_finalize(&h, out.data(), out.size());
    if (size) *size = off;
    return GRAFT_OK;
}

void hash_bytes(const uint8_t *p, size_t n, const Regions &zeroed, Hash &out) {
    blake3_hasher h;
    blake3_hasher_init(&h);
    if (zeroed.empty()) {
        blake3_hasher_update(&h, p, n);
    } else {
        std::vector<uint8_t> copy(p, p + n);
        zero_regions(copy.data(), 0, n, zeroed);
        blake3_hasher_update(&h, copy.data(), n);
    }
    blake3_hasher_finalize(&h, out.data(), out.size());
}

graft_status sync_file(std::FILE *f) {
    if (std::fflush(f) != 0) return GRAFT_E_IO;
#if defined(__unix__) || defined(__APPLE__)
    if (fsync(fileno(f)) != 0) return GRAFT_E_IO;
#endif
    return GRAFT_OK;
}

graft_source FileSource::source() {
    return {this, size, [](void *ctx, uint64_t off, void *buf, size_t len) {
                auto *self = static_cast<FileSource *>(ctx);
                if (!file_seek(self->f, off)) return GRAFT_E_IO;
                return std::fread(buf, 1, len, self->f) == len ? GRAFT_OK : GRAFT_E_IO;
            }};
}

graft_reader RangeReader::reader() {
    return {this, [](void *ctx, void *buf, size_t cap, size_t *got) {
                auto *self = static_cast<RangeReader *>(ctx);
                *got = 0;
                if (!self->positioned) {
                    if (!file_seek(self->f, self->pos)) return GRAFT_E_IO;
                    self->positioned = true;
                }
                const size_t want = static_cast<size_t>(std::min<uint64_t>(cap, self->end - self->pos));
                if (want == 0) return GRAFT_OK;
                const size_t n = std::fread(buf, 1, want, self->f);
                if (n != want) return GRAFT_E_IO;  // the range was checked against the file size
                self->pos += n;
                *got = n;
                return GRAFT_OK;
            }};
}

graft_sink FileSink::sink() {
    return {this, [](void *ctx, const void *buf, size_t len) {
                auto *self = static_cast<FileSink *>(ctx);
                return std::fwrite(buf, 1, len, self->f) == len ? GRAFT_OK : GRAFT_E_IO;
            }};
}

bool valid_rel_path(const std::string &p) {
    if (p.empty() || p.size() > 4096 || p.front() == '/' || p.back() == '/') return false;
    for (char ch : p) {
        const auto c = static_cast<unsigned char>(ch);
        if (c < 0x20 || c == 0x7f || c == '\\') return false;
    }
    size_t start = 0;
    while (start <= p.size()) {
        size_t slash = p.find('/', start);
        if (slash == std::string::npos) slash = p.size();
        const std::string_view comp(p.data() + start, slash - start);
        if (comp.empty() || comp == "." || comp == "..") return false;
        // Drive letters and alternate data streams on Windows.
        if (comp.find(':') != std::string_view::npos) return false;
        start = slash + 1;
    }
    return true;
}

std::string fold_case(const std::string &p) {
    std::string r = p;
    for (char &c : r)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return r;
}

}  // namespace graft
