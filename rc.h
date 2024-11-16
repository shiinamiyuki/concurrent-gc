#pragma once
#include <memory>
namespace rc {
template<class T>
struct RcPtr {
private:
    struct ControlBlock {
        T ptr{};
        size_t ref_count{};
    };
    ControlBlock *control_block_;
    void dec() {
        if (control_block_ && --control_block_->ref_count == 0) {
            delete control_block_;
        }
    }
public:
    RcPtr() : control_block_(nullptr) {}
    template<class... Args>
    explicit RcPtr(Args &&...args) : control_block_(new ControlBlock{std::forward<Args>(args)..., 1}) {}
    RcPtr(const RcPtr &other) : control_block_(other.control_block_) {
        if (control_block_) {
            control_block_->ref_count++;
        }
    }
    RcPtr(RcPtr &&other) noexcept : control_block_(other.control_block_) {
        other.control_block_ = nullptr;
    }
    RcPtr &operator=(const RcPtr &other) {
        if (this == &other) {
            return *this;
        }
        if (control_block_ == other.control_block_) {
            return *this;
        }
        dec();
        control_block_ = other.control_block_;
        if (control_block_) {
            control_block_->ref_count++;
        }
        return *this;
    }
    RcPtr &operator=(RcPtr &&other) noexcept {
        if (this == &other) {
            return *this;
        }
        if (control_block_ == other.control_block_) {
            return *this;
        }
        dec();
        control_block_ = other.control_block_;
        other.control_block_ = nullptr;
        return *this;
    }
    T *operator->() const {
        return &control_block_->ptr;
    }
    T &operator*() const {
        return control_block_->ptr;
    }
    ~RcPtr() {
        dec();
    }
};
}// namespace rc
