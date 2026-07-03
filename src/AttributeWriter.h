#pragma once

#include <string>
#include <vector>

#include "Attribute.h"


namespace bfs {


// Write attributes onto an already-created filesystem node at 'path'. On Haiku
// this restores them into the node's attribute namespace; on other platforms it
// is a no-op (POSIX extraction restores data and metadata only).
void WriteNodeAttributes(const std::string &path,
	const std::vector<Attribute> &attributes);


} // bfs
