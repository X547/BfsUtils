#pragma once

#include <memory>
#include <string>

#include "Node.h"


namespace bfs {


// Recursively scan 'rootPath' (which must be a directory) into an in-memory
// tree. The returned Node is the root directory. When 'readAttributes' is set,
// each node's BFS attributes are read as well (effective on Haiku only).
std::unique_ptr<Node> ScanSource(const std::string &rootPath, bool readAttributes);


} // bfs
