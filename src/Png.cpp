#include "Png.h"

#include <stdio.h>

#include <stdexcept>

#include <png.h>


namespace bfs {


void WritePng(const std::string &path, const Image &image)
{
	if (image.width <= 0 || image.height <= 0) {
		throw std::runtime_error("refusing to write an empty image");
	}
	if (image.rgb.size() != static_cast<size_t>(image.width) * image.height * 3) {
		throw std::runtime_error("image buffer size does not match its dimensions");
	}

	FILE *file = ::fopen(path.c_str(), "wb");
	if (file == nullptr) {
		throw std::runtime_error("cannot open '" + path + "' for writing");
	}

	png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING,
		nullptr, nullptr, nullptr);
	if (png == nullptr) {
		::fclose(file);
		throw std::runtime_error("png_create_write_struct failed");
	}
	png_infop info = png_create_info_struct(png);
	if (info == nullptr) {
		png_destroy_write_struct(&png, nullptr);
		::fclose(file);
		throw std::runtime_error("png_create_info_struct failed");
	}

	// libpng reports errors via longjmp back to this point.
	if (setjmp(png_jmpbuf(png))) {
		png_destroy_write_struct(&png, &info);
		::fclose(file);
		throw std::runtime_error("libpng failed while writing '" + path + "'");
	}

	png_init_io(png, file);
	png_set_IHDR(png, info, image.width, image.height, 8, PNG_COLOR_TYPE_RGB,
		PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
	png_write_info(png, info);

	std::vector<png_bytep> rows(image.height);
	// The cast drops const, but libpng does not modify the row data on write.
	uint8_t *base = const_cast<uint8_t *>(image.rgb.data());
	for (int y = 0; y < image.height; y++) {
		rows[y] = base + static_cast<size_t>(y) * image.width * 3;
	}
	png_write_image(png, rows.data());
	png_write_end(png, nullptr);

	png_destroy_write_struct(&png, &info);
	::fclose(file);
}


} // bfs
