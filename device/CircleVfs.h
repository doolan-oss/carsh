// CircleVfs — device backend for hal::Vfs/IStream/Source over FatFs (PLAN.md
// §2.7). The kernel mounts the SD/USB volume once (f_mount); this exposes it to
// the indexer, decoders, and tag parsers through the same interface the host
// build satisfies with HostVfs. FAT32 only (no exFAT — PLAN.md R6).
//
// Logical paths ("/carsh/db/tracks.db") are prefixed with the FatFs drive
// ("SD:") so the rest of the system uses the same paths it would on the host.
#ifndef _carsh_circle_vfs_h
#define _carsh_circle_vfs_h

#include <fatfs/ff.h>

#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "CircleDir.h"  // directory listing isolated from the DIR collision
#include "hal/Vfs.h"

namespace carsh::device {

class CircleFile : public carsh::hal::IStream {
public:
    ~CircleFile() override { if (open_) f_close(&fil_); }

    bool openRead(const std::string& fatPath) {
        if (f_open(&fil_, fatPath.c_str(), FA_READ) != FR_OK) return false;
        open_ = true;
        size_ = f_size(&fil_);
        return true;
    }
    std::size_t read(void* buf, std::size_t n) override {
        UINT br = 0;
        if (f_read(&fil_, buf, UINT(n), &br) != FR_OK) return 0;
        return br;
    }
    bool seek(std::uint64_t pos) override {
        return f_lseek(&fil_, FSIZE_t(pos)) == FR_OK;
    }
    std::uint64_t tell() const override { return f_tell(&fil_); }
    std::uint64_t size() const override { return size_; }

private:
    FIL fil_{};
    std::uint64_t size_ = 0;
    bool open_ = false;
};

class CircleVfs : public carsh::hal::Vfs {
public:
    explicit CircleVfs(std::string drive = "SD:") : drive_(std::move(drive)) {}

    std::unique_ptr<carsh::hal::IStream> open(const std::string& path) override {
        auto f = std::make_unique<CircleFile>();
        if (!f->openRead(toFat(path))) return nullptr;
        return f;
    }

    bool stat(const std::string& path, carsh::hal::DirEntry& out) override {
        FfEntry e;
        if (!ffStat(toFat(path).c_str(), e)) return false;
        out.name = e.name; out.isDir = e.isDir; out.size = e.size;
        out.mtime = e.mtime;
        return true;
    }

    bool readdir(const std::string& path,
                 std::vector<carsh::hal::DirEntry>& out) override {
        std::vector<FfEntry> entries;
        if (!ffListDir(toFat(path).c_str(), entries)) return false;
        for (auto& e : entries) {
            carsh::hal::DirEntry de;
            de.name = e.name; de.isDir = e.isDir; de.size = e.size;
            de.mtime = e.mtime;
            out.push_back(std::move(de));
        }
        return true;
    }

private:
    std::string toFat(const std::string& logical) const {
        if (logical.empty() || logical == "/") return drive_ + "/";
        std::string p = logical;
        if (p.front() != '/') p = "/" + p;
        return drive_ + p;
    }
    std::string drive_;
};

class CircleSource : public carsh::hal::Source {
public:
    CircleSource(std::uint8_t id, carsh::hal::SourceType type,
                 const std::string& drive)
        : id_(id), type_(type), vfs_(drive) {}
    std::uint8_t id() const override { return id_; }
    carsh::hal::SourceType type() const override { return type_; }
    carsh::hal::Vfs& vfs() override { return vfs_; }
    std::string root() const override { return "/"; }

private:
    std::uint8_t id_;
    carsh::hal::SourceType type_;
    CircleVfs vfs_;
};

}  // namespace carsh::device

#endif
