#pragma once

#include <string>
#include <vector>

#include "Node.h"


namespace bfs {


// Read a node's BFS attributes into 'out'. On Haiku this enumerates the file's
// attribute namespace; on other platforms it is a no-op (POSIX builds archive
// data and the standard indices only, per project requirements).
void ReadNodeAttributes(const std::string &path, std::vector<Attribute> &out);


} // bfs
