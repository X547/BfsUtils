#pragma once

#include <memory>
#include <string>

#include "Node.h"
#include "Progress.h"


namespace bfs {


// Recursively scan 'rootPath' (which must be a directory) into an in-memory
// tree. The returned Node is the root directory. When 'readAttributes' is set,
// each node's BFS attributes are read as well (effective on Haiku only).
// 'progress' counts entries as they are visited; the extent is unknown until
// the walk finishes, so the phase reports a running count.
std::unique_ptr<Node> ScanSource(const std::string &rootPath, bool readAttributes,
	Progress &progress);


} // bfs
