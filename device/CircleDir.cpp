// Implementation of the directory wrapper. This TU includes ONLY ff.h (plus the
// minimal STL it needs) so FatFs's `DIR` type is unambiguous here.
#include "CircleDir.h"

#include <fatfs/ff.h>

namespace carsh::device {

static void fill(const FILINFO& fi, FfEntry& e) {
    e.name = fi.fname;
    e.isDir = (fi.fattrib & AM_DIR) != 0;
    e.size = fi.fsize;
    e.mtime = (std::uint32_t(fi.fdate) << 16) | fi.ftime;
}

bool ffListDir(const char* fatPath, std::vector<FfEntry>& out) {
    DIR dir;
    if (f_opendir(&dir, fatPath) != FR_OK) return false;
    FILINFO fi;
    for (;;) {
        if (f_readdir(&dir, &fi) != FR_OK || fi.fname[0] == 0) break;
        FfEntry e;
        fill(fi, e);
        out.push_back(e);
    }
    f_closedir(&dir);
    return true;
}

bool ffStat(const char* fatPath, FfEntry& out) {
    FILINFO fi;
    if (f_stat(fatPath, &fi) != FR_OK) return false;
    fill(fi, out);
    return true;
}

}  // namespace carsh::device
