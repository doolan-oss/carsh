// Directory listing isolated from the FatFs/POSIX `DIR` type collision. FatFs
// defines a `DIR` type and newlib defines a POSIX `DIR`; when both are visible
// (Circle's app headers pull <dirent.h>) the FatFs one is shadowed. This wrapper
// is compiled in a TU that sees only ff.h, exposing a plain listing API.
#ifndef _carsh_circle_dir_h
#define _carsh_circle_dir_h

#include <cstdint>
#include <string>
#include <vector>

namespace carsh::device {

struct FfEntry {
    std::string name;
    bool isDir = false;
    std::uint64_t size = 0;
    std::uint32_t mtime = 0;
};

// List `fatPath` (e.g. "SD:/carsh"). Returns false if it is not a directory.
bool ffListDir(const char* fatPath, std::vector<FfEntry>& out);

// Stat a single path into `out`. Returns false if absent.
bool ffStat(const char* fatPath, FfEntry& out);

}  // namespace carsh::device

#endif
