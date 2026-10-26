// Pixel-art upscaling with xBRZ (deps/xbrzscale/xbrz, GPLv3).
#include <cstdint>
#include <vector>

#include "xbrz.h"

extern "C" void rcUpscale(const uint32_t* src, uint32_t* dst, int width, int height, int factor) {
	// The game's frame is 0x00BBGGRR; xBRZ wants 0x00RRGGBB and returns the same
	static std::vector<uint32_t> rgb;
	rgb.resize(width * height);
	for (int i = 0; i < width * height; ++i) {
		uint32_t c = src[i];
		rgb[i] = ((c & 0xFF) << 16) | (c & 0xFF00) | ((c >> 16) & 0xFF);
	}
	xbrz::scale(factor, rgb.data(), dst, width, height, xbrz::ColorFormat::RGB);
}
