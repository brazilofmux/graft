#include "masks.h"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>

namespace cli {
namespace {

bool parse_num(const std::string &s, uint64_t &v) {
    if (s.empty() || s[0] == '-') return false;
    char *end;
    errno = 0;
    v = std::strtoull(s.c_str(), &end, 0);
    return errno == 0 && *end == '\0';
}

bool parse_signed(const std::string &s, int64_t &v) {
    if (s.empty()) return false;
    char *end;
    errno = 0;
    v = std::strtoll(s.c_str(), &end, 0);
    return errno == 0 && *end == '\0';
}

bool parse_range(const std::string &s, uint64_t &off, uint64_t &len) {
    const auto colon = s.find(':');
    return colon != std::string::npos && parse_num(s.substr(0, colon), off) && parse_num(s.substr(colon + 1), len);
}

bool parse_hex(const std::string &s, std::string &out) {
    if (s.size() % 2) return false;
    out.clear();
    for (size_t i = 0; i < s.size(); i += 2) {
        char *end;
        const std::string byte = s.substr(i, 2);
        const long v = std::strtol(byte.c_str(), &end, 16);
        if (*end != '\0') return false;
        out.push_back(static_cast<char>(v));
    }
    return true;
}

// Offsets of every occurrence (stops after two: we only need "exactly one").
std::vector<uint64_t> find_marker(const uint8_t *p, size_t n, const std::string &m) {
    std::vector<uint64_t> hits;
    const auto *mb = reinterpret_cast<const uint8_t *>(m.data());
    const std::boyer_moore_horspool_searcher searcher(mb, mb + m.size());
    for (const uint8_t *at = p; hits.size() < 2;) {
        const uint8_t *hit = std::search(at, p + n, searcher);
        if (hit == p + n) break;
        hits.push_back(static_cast<uint64_t>(hit - p));
        at = hit + 1;
    }
    return hits;
}

}  // namespace

bool resolve_mask(const std::string &spec, const uint8_t *oldp, size_t oldn, const uint8_t *newp, size_t newn,
                  graft_mask &out, std::string &err) {
    std::optional<std::pair<uint64_t, uint64_t>> at, old_r, new_r;
    std::optional<std::string> marker;
    std::optional<uint64_t> len, newlen;
    std::optional<int64_t> skip;
    std::optional<uint8_t> pad;
    bool truncate = false;

    size_t start = 0;
    while (start <= spec.size()) {
        size_t comma = spec.find(',', start);
        if (comma == std::string::npos) comma = spec.size();
        const std::string item = spec.substr(start, comma - start);
        start = comma + 1;
        if (item.empty()) continue;
        const auto eq = item.find('=');
        const std::string key = item.substr(0, eq), val = eq == std::string::npos ? "" : item.substr(eq + 1);
        uint64_t a, b;
        if (key == "at" && parse_range(val, a, b)) at = {a, b};
        else if (key == "old" && parse_range(val, a, b)) old_r = {a, b};
        else if (key == "new" && parse_range(val, a, b)) new_r = {a, b};
        else if (key == "marker" && !val.empty()) {
            std::string m = val;
            if (val.rfind("hex:", 0) == 0 && !parse_hex(val.substr(4), m)) {
                err = "bad hex in '" + item + "'";
                return false;
            }
            if (m.empty()) {
                err = "empty marker";
                return false;
            }
            marker = m;
        } else if (key == "len" && parse_num(val, a)) len = a;
        else if (key == "newlen" && parse_num(val, a)) newlen = a;
        else if (key == "skip") {
            int64_t k;
            if (!parse_signed(val, k)) {
                err = "bad value in '" + item + "'";
                return false;
            }
            skip = k;
        } else if (key == "pad" && parse_num(val, a) && a <= 0xff) pad = static_cast<uint8_t>(a);
        else if (key == "truncate" && eq == std::string::npos) truncate = true;
        else {
            err = "bad item '" + item + "'";
            return false;
        }
    }

    const int ways = (at ? 1 : 0) + ((old_r || new_r) ? 1 : 0) + (marker ? 1 : 0);
    if (ways != 1) {
        err = "give exactly one of at=, old=/new=, or marker=";
        return false;
    }

    uint64_t oo, ol, no, nl;
    if (at) {
        oo = no = at->first;
        ol = at->second;
        nl = newlen.value_or(ol);
    } else if (old_r || new_r) {
        if (!old_r || !new_r) {
            err = "old= and new= go together";
            return false;
        }
        oo = old_r->first;
        ol = old_r->second;
        no = new_r->first;
        nl = new_r->second;
    } else {
        if (!len) {
            err = "marker= needs len=";
            return false;
        }
        const auto ho = find_marker(oldp, oldn, *marker), hn = find_marker(newp, newn, *marker);
        for (const auto &[hits, which] : {std::pair{&ho, "OLD"}, std::pair{&hn, "NEW"}}) {
            if (hits->empty()) {
                err = std::string("marker not found in ") + which;
                return false;
            }
            if (hits->size() > 1) {
                err = std::string("marker occurs more than once in ") + which + " (it must be unique)";
                return false;
            }
        }
        const int64_t k = skip.value_or(static_cast<int64_t>(marker->size()));
        const auto place = [&](uint64_t hit, uint64_t &off) {
            if (k < 0 && static_cast<uint64_t>(-k) > hit) return false;
            off = static_cast<uint64_t>(static_cast<int64_t>(hit) + k);
            return true;
        };
        if (!place(ho[0], oo) || !place(hn[0], no)) {
            err = "skip= puts the region before the start of the file";
            return false;
        }
        ol = *len;
        nl = newlen.value_or(ol);
    }

    if (pad && truncate) {
        err = "pad= and truncate are exclusive";
        return false;
    }
    uint8_t mode = GRAFT_MASK_EXACT;
    if (ol != nl) {
        if (pad && nl > ol) mode = GRAFT_MASK_PAD;
        else if (truncate && nl < ol) mode = GRAFT_MASK_TRUNCATE;
        else {
            err = "region changes size (" + std::to_string(ol) + " -> " + std::to_string(nl) + " bytes); add " +
                  (nl > ol ? "pad=BYTE" : "truncate") + " to say how";
            return false;
        }
    }
    if (oo > oldn || ol > oldn - oo) {
        err = "region extends past the end of OLD";
        return false;
    }
    if (no > newn || nl > newn - no) {
        err = "region extends past the end of NEW";
        return false;
    }
    out = {oo, ol, no, nl, mode, pad.value_or(0)};
    return true;
}

bool read_mask_file(const char *path, std::vector<std::string> &specs, std::string &err) {
    std::ifstream f(std::filesystem::path(std::u8string(reinterpret_cast<const char8_t *>(path))));
    if (!f) {
        err = std::string(path) + ": " + std::strerror(errno);
        return false;
    }
    std::string line;
    while (std::getline(f, line)) {
        if (const auto hash = line.find('#'); hash != std::string::npos) line.erase(hash);
        const auto b = line.find_first_not_of(" \t\r"), e = line.find_last_not_of(" \t\r");
        if (b != std::string::npos) specs.push_back(line.substr(b, e - b + 1));
    }
    return true;
}

}  // namespace cli
