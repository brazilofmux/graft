// Every filter must be exactly reversible on any input, whole-buffer and
// streamed in arbitrary chunks.
// Input: [filter selector][chunking seed][bytes...]
#include "filter.h"

#include <cstdlib>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size < 2) return 0;
    const uint8_t id = static_cast<uint8_t>(1 + data[0] % 5);
    uint32_t seed = data[1] | 1u;
    const std::vector<uint8_t> orig(data + 2, data + size);

    std::vector<uint8_t> enc = orig;
    graft::filter_encode(id, enc.data(), enc.size());
    std::vector<uint8_t> dec = enc;
    graft::filter_decode(id, dec.data(), dec.size());
    if (dec != orig) std::abort();

    graft::FilterDecoder d(id);
    std::vector<uint8_t> out;
    for (size_t i = 0; i < enc.size();) {
        seed = seed * 1103515245u + 12345u;
        const size_t n = std::min<size_t>(enc.size() - i, 1 + (seed >> 16) % 11);
        d.push(enc.data() + i, n, out);
        i += n;
    }
    d.finish(out);
    if (out != orig) std::abort();
    return 0;
}
