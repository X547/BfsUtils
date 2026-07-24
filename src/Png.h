#pragma once

#include <stdint.h>

#include <string>
#include <vector>


namespace bfs {


// A simple 24-bit RGB image, one byte each for R, G, B, stored row-major with
// no padding (stride == width * 3).
struct Image {
	int width = 0;
	int height = 0;
	std::vector<uint8_t> rgb;   // size == width * height * 3

	void Allocate(int w, int h)
	{
		width = w;
		height = h;
		rgb.assign(static_cast<size_t>(w) * h * 3, 0);
	}

	void SetPixel(int x, int y, uint8_t r, uint8_t g, uint8_t b)
	{
		if (x < 0 || y < 0 || x >= width || y >= height) {
			return;
		}
		size_t i = (static_cast<size_t>(y) * width + x) * 3;
		rgb[i + 0] = r;
		rgb[i + 1] = g;
		rgb[i + 2] = b;
	}

	void FillRect(int x, int y, int w, int h, uint8_t r, uint8_t g, uint8_t b)
	{
		for (int yy = y; yy < y + h; yy++) {
			for (int xx = x; xx < x + w; xx++) {
				SetPixel(xx, yy, r, g, b);
			}
		}
	}
};


// Write 'image' as a PNG file. Throws std::runtime_error on failure.
void WritePng(const std::string &path, const Image &image);


} // bfs
