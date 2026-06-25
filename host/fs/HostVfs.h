// HostVfs / HostSource — Vfs backed by a real directory on the build server.
//
// Lets the indexer, decoders, and tag parsers run over real media fixtures. A
// "mount root" on the host maps to the Source's logical root ("/"), so paths the
// rest of the system sees match what Circle's FAT mount would present.
#pragma once

#include <cstdio>
#include <dirent.h>
#include <sys/stat.h>

#include <memory>
#include <string>
#include <vector>

#include "hal/Vfs.h"

namespace carsh::host {

class HostStream : public carsh::hal::IStream {
public:
    explicit HostStream(FILE* f, std::uint64_t size) : f_(f), size_(size) {}
    ~HostStream() override { if (f_) std::fclose(f_); }

    std::size_t read(void* buf, std::size_t n) override {
        std::size_t r = std::fread(buf, 1, n, f_);
        pos_ += r;
        return r;
    }
    bool seek(std::uint64_t pos) override {
        if (std::fseek(f_, long(pos), SEEK_SET) != 0) return false;
        pos_ = pos;
        return true;
    }
    std::uint64_t tell() const override { return pos_; }
    std::uint64_t size() const override { return size_; }

private:
    FILE* f_ = nullptr;
    std::uint64_t pos_ = 0;
    std::uint64_t size_ = 0;
};

class HostVfs : public carsh::hal::Vfs {
public:
    explicit HostVfs(std::string mountRoot) : root_(std::move(mountRoot)) {
        if (!root_.empty() && root_.back() == '/') root_.pop_back();
    }

    std::unique_ptr<carsh::hal::IStream> open(const std::string& path) override {
        std::string real = realPath(path);
        struct stat st {};
        if (::stat(real.c_str(), &st) != 0 || S_ISDIR(st.st_mode))
            return nullptr;
        FILE* f = std::fopen(real.c_str(), "rb");
        if (!f) return nullptr;
        return std::make_unique<HostStream>(f, std::uint64_t(st.st_size));
    }

    bool stat(const std::string& path, carsh::hal::DirEntry& out) override {
        struct stat st {};
        if (::stat(realPath(path).c_str(), &st) != 0) return false;
        out.name = basename(path);
        out.isDir = S_ISDIR(st.st_mode);
        out.size = std::uint64_t(st.st_size);
        out.mtime = std::uint64_t(st.st_mtime);
        return true;
    }

    bool readdir(const std::string& path,
                 std::vector<carsh::hal::DirEntry>& out) override {
        std::string real = realPath(path);
        DIR* d = ::opendir(real.c_str());
        if (!d) return false;
        struct dirent* e;
        while ((e = ::readdir(d)) != nullptr) {
            std::string name = e->d_name;
            if (name == "." || name == "..") continue;
            carsh::hal::DirEntry de;
            std::string child = path;
            if (child.empty() || child.back() != '/') child += '/';
            child += name;
            if (stat(child, de)) out.push_back(std::move(de));
        }
        ::closedir(d);
        return true;
    }

    const std::string& mountRoot() const { return root_; }

private:
    std::string realPath(const std::string& logical) const {
        if (logical.empty() || logical == "/") return root_;
        std::string p = logical;
        if (p.front() != '/') p = "/" + p;
        return root_ + p;
    }
    static std::string basename(const std::string& p) {
        auto pos = p.find_last_of('/');
        return pos == std::string::npos ? p : p.substr(pos + 1);
    }
    std::string root_;
};

class HostSource : public carsh::hal::Source {
public:
    HostSource(std::uint8_t id, carsh::hal::SourceType type, std::string root)
        : id_(id), type_(type), vfs_(std::move(root)) {}

    std::uint8_t id() const override { return id_; }
    carsh::hal::SourceType type() const override { return type_; }
    carsh::hal::Vfs& vfs() override { return vfs_; }
    std::string root() const override { return "/"; }

private:
    std::uint8_t id_;
    carsh::hal::SourceType type_;
    HostVfs vfs_;
};

}  // namespace carsh::host
