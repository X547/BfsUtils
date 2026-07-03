#pragma once

#include <stdint.h>

#include <string>
#include <vector>


namespace bfs {


// A BFS attribute: a named, typed blob. 'type' is the BFS/BeOS type code, copied
// through verbatim in both directions (archive and extract). Used by both the
// writer (makebfs) and the reader (bfsextract).
struct Attribute {
	std::string name;
	uint32_t type = 0;
	std::vector<uint8_t> data;
};


} // bfs
