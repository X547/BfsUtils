#pragma once

#include <stdint.h>

#include <string>

#include "BlockMap.h"
#include "Png.h"


namespace bfs {


struct RenderOptions {
	int blocksPerRow = 0;   // 0 == auto
	int scale = 0;          // pixels per block cell; 0 == auto
};


// Renders a classified BlockMap into an RGB image: a header line, the colored
// block grid (row-major, one cell per block), and a legend listing each block
// type present with its block count and share of the volume.
Image RenderBlockMap(const BlockMap &map, const std::string &title,
	uint32_t blockSize, int64_t usedBlocks, const RenderOptions &options);


} // bfs
