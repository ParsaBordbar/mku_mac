// C entry point to xBRZ (third_party/xbrz, GPL-3.0). Pixels are RGBA8888 with
// R in the low byte (our texture cache's layout); xBRZ wants 0xAARRGGBB, so
// red and blue are swapped around the call (its colour distance weights them
// differently).
#include "xbrz.h"

#include <cstdint>
#include <vector>

static inline uint32_t swap_rb(uint32_t p) { return (p & 0xFF00FF00u) | ((p >> 16) & 0xFFu) | ((p & 0xFFu) << 16); }

extern "C" void xbrz_scale_rgba(int factor, const uint32_t *src, uint32_t *dst, int w, int h) {
    std::vector<uint32_t> in((size_t)w * h);
    for (size_t i = 0; i < in.size(); i++) in[i] = swap_rb(src[i]);
    xbrz::scale((size_t)factor, in.data(), dst, w, h, xbrz::ColorFormat::ARGB);
    for (size_t i = 0; i < (size_t)w * h * factor * factor; i++) dst[i] = swap_rb(dst[i]);
}
