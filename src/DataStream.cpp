#include "DataStream.h"

#include <string.h>

#include <stdexcept>


namespace bfs {


void WriteDataStream(uint8_t *streamFields, const Geometry &geometry,
	const std::vector<BlockRun> &runs, uint64_t size)
{
	::memset(streamFields, 0, 144);

	if (runs.size() > static_cast<size_t>(kNumDirectBlocks)) {
		throw std::runtime_error(
			"file requires indirect blocks, which are not yet supported");
	}

	uint64_t directBytes = 0;
	for (size_t i = 0; i < runs.size(); i++) {
		PutBlockRun(streamFields + stream::kDirect + i * 8, runs[i]);
		directBytes += static_cast<uint64_t>(runs[i].length) << geometry.blockShift;
	}

	// With only direct runs, max_direct_range covers all data; the indirect and
	// double-indirect ranges stay zero.
	PutS64(streamFields + stream::kMaxDirectRange, static_cast<int64_t>(directBytes));
	PutS64(streamFields + stream::kMaxIndirectRange, 0);
	PutS64(streamFields + stream::kMaxDoubleIndirectRange, 0);
	PutS64(streamFields + stream::kSize, static_cast<int64_t>(size));
}


} // bfs
