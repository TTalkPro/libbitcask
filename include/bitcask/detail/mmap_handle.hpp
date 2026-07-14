// MmapRegion — mmap 区域的 RAII 句柄（S=RISK_REPORT MED-1）。
//
// 持有 {void* ptr, size_t len}，析构统一 munmap。覆盖封口段、DataFile 只读 mmap、
// HNSW payload、IVF/DiskANN 段等的 munmap 释放样板。fd 生命周期由调用方另行
// 管理（有的 mmap 后立即关 fd、有的持 fd 直至析构——契约不统一，不入本类）。
//
// 析构序约定：mmap 区域必须在 close(fd) **之前** munmap（POSIX：fd 关闭后
// 同文件其他进程的 mmap 行为未定义）。caller 用显式 reset() 控制序，或依赖
// 成员声明序（MmapRegion 成员声明在 fd 成员之前 → 析构反序：fd 先 close、
// mmap 后 munmap——这是错的！故必须显式 reset 或把 fd 也 RAII）。

#pragma once

#include <cstddef>
#include <sys/mman.h>  // ::munmap

namespace bitcask::detail {

class MmapRegion {
public:
    MmapRegion() noexcept = default;
    MmapRegion(void* ptr, std::size_t len) noexcept : ptr_(ptr), len_(len) {}

    ~MmapRegion() { reset(); }

    MmapRegion(const MmapRegion&) = delete;
    MmapRegion& operator=(const MmapRegion&) = delete;

    MmapRegion(MmapRegion&& o) noexcept : ptr_(o.ptr_), len_(o.len_) {
        o.ptr_ = nullptr;
        o.len_ = 0;
    }

    MmapRegion& operator=(MmapRegion&& o) noexcept {
        if (this != &o) {
            reset();
            ptr_ = o.ptr_;
            len_ = o.len_;
            o.ptr_ = nullptr;
            o.len_ = 0;
        }
        return *this;
    }

    void reset() noexcept {
        if (ptr_ != nullptr) {
            ::munmap(ptr_, len_);
            ptr_ = nullptr;
            len_ = 0;
        }
    }

    void release(void*& out_ptr, std::size_t& out_len) noexcept {
        out_ptr = ptr_;
        out_len = len_;
        ptr_ = nullptr;
        len_ = 0;
    }

    [[nodiscard]] void* get() noexcept { return ptr_; }
    [[nodiscard]] const void* get() const noexcept { return ptr_; }
    [[nodiscard]] std::size_t size() const noexcept { return len_; }
    [[nodiscard]] bool empty() const noexcept { return ptr_ == nullptr; }
    explicit operator bool() const noexcept { return ptr_ != nullptr; }

private:
    void*       ptr_ = nullptr;
    std::size_t len_ = 0;
};

}  // namespace bitcask::detail
