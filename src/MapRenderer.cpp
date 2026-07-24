#include "MapRenderer.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>

#include <algorithm>

#include "Font5x7.h"


namespace bfs {


namespace {


struct Rgb {
	uint8_t r, g, b;
};

const Rgb kBackground = {0x12, 0x14, 0x1a};
const Rgb kText = {0xd0, 0xd3, 0xd8};
const Rgb kGridBorder = {0x2c, 0x30, 0x38};

const int kGlyphWidth = 5;
const int kGlyphHeight = 7;
const int kGlyphAdvance = 6;   // glyph width + 1 column of spacing


void DrawChar(Image &image, int x, int y, char c, int scale, const Rgb &color)
{
	const uint8_t *rows = Font5x7Glyph(c);
	for (int ry = 0; ry < kGlyphHeight; ry++) {
		uint8_t bits = rows[ry];
		for (int rx = 0; rx < kGlyphWidth; rx++) {
			if ((bits >> (kGlyphWidth - 1 - rx)) & 1) {
				image.FillRect(x + rx * scale, y + ry * scale, scale, scale,
					color.r, color.g, color.b);
			}
		}
	}
}


void DrawText(Image &image, int x, int y, const std::string &text, int scale,
	const Rgb &color)
{
	int cursor = x;
	for (char c : text) {
		DrawChar(image, cursor, y, c, scale, color);
		cursor += kGlyphAdvance * scale;
	}
}


std::string FormatPercent(int64_t part, int64_t whole)
{
	double pct = whole > 0 ? 100.0 * static_cast<double>(part) / whole : 0.0;
	char buffer[32];
	snprintf(buffer, sizeof(buffer), "%.1f", pct);
	return buffer;
}


} // unnamed namespace


Image RenderBlockMap(const BlockMap &map, const std::string &title,
	uint32_t blockSize, int64_t usedBlocks, const RenderOptions &options)
{
	int64_t numBlocks = std::max<int64_t>(map.NumBlocks(), 1);
	const int64_t *counts = map.Counts();
	const std::vector<uint8_t> &types = map.Types();

	// Choose a row width (in blocks) that gives a roughly 2:1 landscape grid,
	// then a per-block pixel scale that keeps the grid a sensible size.
	int blocksPerRow = options.blocksPerRow;
	if (blocksPerRow <= 0) {
		blocksPerRow = static_cast<int>(ceil(sqrt(static_cast<double>(numBlocks) * 2.0)));
		blocksPerRow = std::max(64, std::min(blocksPerRow, 2048));
	}
	blocksPerRow = std::min<int>(blocksPerRow, static_cast<int>(numBlocks));
	blocksPerRow = std::max(blocksPerRow, 1);

	int scale = options.scale;
	if (scale <= 0) {
		scale = std::max(1, std::min(12, 1024 / blocksPerRow));
	}

	int gridRows = static_cast<int>((numBlocks + blocksPerRow - 1) / blocksPerRow);
	int gridW = blocksPerRow * scale;
	int gridH = gridRows * scale;

	const int margin = 14;
	const int headerScale = 2;
	const int headerLineHeight = kGlyphHeight * headerScale + 6;
	const int legendScale = 2;
	const int legendRowHeight = kGlyphHeight * legendScale + 7;
	const int swatch = kGlyphHeight * legendScale;

	// Header lines.
	char line2[160];
	snprintf(line2, sizeof(line2),
		"%lld BLOCKS X %u BYTES   USED %lld (%s%%)",
		static_cast<long long>(map.NumBlocks()), blockSize,
		static_cast<long long>(usedBlocks),
		FormatPercent(usedBlocks, map.NumBlocks()).c_str());
	std::string header1 = "BFS BLOCK MAP: " + title;
	std::string header2 = line2;

	int legendRows = 0;
	for (int i = 0; i < kBlockTypeCount; i++) {
		if (counts[i] > 0) {
			legendRows++;
		}
	}

	int headerH = 2 * headerLineHeight;
	int legendH = legendRows * legendRowHeight;

	int contentW = std::max(gridW, 520);
	int width = contentW + 2 * margin;
	int height = margin + headerH + 8 + gridH + 12 + legendH + margin;

	Image image;
	image.Allocate(width, height);
	image.FillRect(0, 0, width, height, kBackground.r, kBackground.g, kBackground.b);

	int y = margin;
	DrawText(image, margin, y, header1, headerScale, kText);
	y += headerLineHeight;
	DrawText(image, margin, y, header2, headerScale, kText);
	y += headerLineHeight + 8;

	// Grid border, then the blocks.
	int gridX = margin;
	int gridY = y;
	image.FillRect(gridX - 1, gridY - 1, gridW + 2, gridH + 2,
		kGridBorder.r, kGridBorder.g, kGridBorder.b);
	for (int64_t block = 0; block < numBlocks; block++) {
		int col = static_cast<int>(block % blocksPerRow);
		int row = static_cast<int>(block / blocksPerRow);
		const BlockTypeInfo &info = kBlockTypeInfo[types[block]];
		image.FillRect(gridX + col * scale, gridY + row * scale, scale, scale,
			info.r, info.g, info.b);
	}
	y = gridY + gridH + 12;

	// Legend: one row per present block type.
	for (int i = 0; i < kBlockTypeCount; i++) {
		if (counts[i] == 0) {
			continue;
		}
		const BlockTypeInfo &info = kBlockTypeInfo[i];
		image.FillRect(margin, y, swatch, swatch, info.r, info.g, info.b);
		image.FillRect(margin, y, swatch, 1, kGridBorder.r, kGridBorder.g, kGridBorder.b);

		char label[96];
		std::string name = info.name;
		for (char &c : name) {
			c = static_cast<char>(::toupper(static_cast<unsigned char>(c)));
		}
		snprintf(label, sizeof(label), "%s: %lld (%s%%)", name.c_str(),
			static_cast<long long>(counts[i]),
			FormatPercent(counts[i], map.NumBlocks()).c_str());
		DrawText(image, margin + swatch + 8, y + (swatch - kGlyphHeight * legendScale) / 2,
			label, legendScale, kText);
		y += legendRowHeight;
	}

	return image;
}


} // bfs
