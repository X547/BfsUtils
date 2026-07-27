#pragma once

#include <string>


namespace bfs {


// Whether 'path' names a device that is currently mounted. On success 'where' is
// set to a human-readable phrase identifying the mount, ready to be dropped into
// a sentence -- the two platforms can say different things, because Linux knows
// the mount point and Haiku only records the volume name.
//
// Writing to a mounted volume corrupts it under the running file system, so the
// writers refuse to touch one. Returns false for anything that is not a device,
// and false when the answer cannot be determined -- the check is a guard against
// the obvious accident, not a security boundary.
bool IsDeviceMounted(const std::string &path, std::string &where);


} // bfs
