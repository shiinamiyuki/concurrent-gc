#pragma once
#include <memory>
#include <memory_resource>
namespace rc {
struct RefCounter {
    size_t count{};
    void inc() {
        count++;
    }
    size_t dec() {
        count--;
        return count;
    }
    static std::unique_ptr<std::pmr::memory_resource> resource;
};
struct AtomicRefCounter {
    std::atomic<size_t> count{};
    void inc() {
        count.fetch_add(1, std::memory_order_relaxed);
    }
    size_t dec() {
        return count.fetch_sub(1, std::memory_order_relaxed) - 1;
    }
    static std::unique_ptr<std::pmr::memory_resource> resource;
};

template<class T, class CounterPolicy = RefCounter>
struct RcPtr {
    struct ControlBlock {
        T ptr{};
        CounterPolicy ref_count{};
    };
    ControlBlock *control_block_;
    void dec() {
        if (control_block_ && control_block_->ref_count.dec() == 0) {
            auto alloc = std::pmr::polymorphic_allocator(CounterPolicy::resource.get());
            alloc.destroy(control_block_);
            alloc.deallocate_object(control_block_, 1);
        }
    }

    RcPtr() : control_block_(nullptr) {}
    template<class... Args>
    explicit RcPtr(Args &&...args) {
        auto alloc = std::pmr::polymorphic_allocator(CounterPolicy::resource.get());
        control_block_ = alloc.allocate_object<ControlBlock>(1);
        alloc.construct(control_block_, std::forward<Args>(args)...);
    }
    RcPtr(const RcPtr &other) : control_block_(other.control_block_) {
        if (control_block_) {
            control_block_->ref_count.inc();
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
            control_block_->ref_count.inc();
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

template<class T, class CounterPolicy = RefCounter>
struct RcFromThis {
private:
    using cb_t = RcPtr<T, CounterPolicy>::ControlBlock;
    cb_t *control_block_;
protected:
    RcPtr<T, CounterPolicy> rc_from_this() {
        auto ptr = RcPtr<T, CounterPolicy>;
        ptr.control_block_ = control_block_;
        if (control_block_) {
            control_block_->ref_count.inc();
        } else {
            std::abort();
        }
        return ptr;
    }
};

template<class T, class CounterPolicy = RefCounter>
struct DummyMember : public RcPtr<T, CounterPolicy> {
};
}// namespace rc
