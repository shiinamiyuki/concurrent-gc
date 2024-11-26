#pragma once
#include <memory>
#include <memory_resource>
#include "gc.h"
namespace rc {
struct RefCounter {
    int64_t count{};
    void inc() {
        GC_ASSERT(count >= 0, "Count should be positive");
        count++;
    }
    int64_t dec() {
        count--;
        GC_ASSERT(count >= 0, "Count should be positive");
        return count;
    }
    static std::unique_ptr<std::pmr::memory_resource> resource;
};
struct AtomicRefCounter {
    std::atomic<int64_t> count{};
    void inc() {
        GC_ASSERT(count >= 0, "Count should be positive");
        count.fetch_add(1, std::memory_order_acquire);
    }
    int64_t dec() {
        GC_ASSERT(count >= 0, "Count should be positive");
        return count.fetch_sub(1, std::memory_order_release) - 1;
    }
    static std::unique_ptr<std::pmr::memory_resource> resource;
};
template<class T, class CounterPolicy>
struct RcFromThis;
template<class T, class CounterPolicy = RefCounter>
struct RcPtr {
    struct ControlBlock {
        T ptr{};
        CounterPolicy ref_count{};
        template<class... Args>
        ControlBlock(Args &&...args) : ptr(std::forward<Args>(args)...) {}
    };
    ControlBlock *control_block_;
    void dec() {
        if (control_block_ && control_block_->ref_count.dec() == 0) {
            auto alloc = std::pmr::polymorphic_allocator<>(CounterPolicy::resource.get());
            std::allocator_traits<std::pmr::polymorphic_allocator<>>::destroy(alloc, control_block_);
            // std::printf("deleting control block: %p\n", static_cast<void *>(control_block_));
            alloc.deallocate_object(control_block_, 1);
        }
    }

    RcPtr() : control_block_(nullptr) {}
    struct init_t {};
    template<class... Args>
    explicit RcPtr(init_t, Args &&...args) {
        auto alloc = std::pmr::polymorphic_allocator(CounterPolicy::resource.get());
        control_block_ = alloc.template allocate_object<ControlBlock>(1);
        alloc.construct(control_block_, std::forward<Args>(args)...);
        // std::printf("created control block: %p\n", static_cast<void *>(control_block_));
        control_block_->ref_count.inc();
        if constexpr (std::is_base_of_v<RcFromThis<T, CounterPolicy>, T>) {
            control_block_->ptr.control_block_ = control_block_;
          
        }
    }
    RcPtr(const RcPtr &other) : control_block_(other.control_block_) {
        if (control_block_) {
            control_block_->ref_count.inc();
        }
    }
    RcPtr(RcPtr &&other) noexcept : control_block_(other.control_block_) {
        other.control_block_ = nullptr;
    }
    RcPtr &operator=(std::nullptr_t) {
        dec();
        control_block_ = nullptr;
        return *this;
    }
    template<typename U = void>
        requires std::is_base_of_v<RcFromThis<T, CounterPolicy>, T>
    RcPtr &operator=(T *ptr) {
        auto rc = ptr->rc_from_this();
        *this = rc;
        return *this;
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
    template<typename U, typename V>
    friend struct RcPtr;
private:
    using cb_t = RcPtr<T, CounterPolicy>::ControlBlock;
    cb_t *control_block_;
protected:
    RcPtr<T, CounterPolicy> rc_from_this() {
        auto ptr = RcPtr<T, CounterPolicy>();
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
    template<class U>
    DummyMember(U *_parent) {}
    using Base = RcPtr<T, CounterPolicy>;
    DummyMember(const DummyMember &other) = delete;
    DummyMember(DummyMember &&other) = delete;
    DummyMember &operator=(const DummyMember &other) {
        Base::operator=(other);
        return *this;
    }
    DummyMember &operator=(DummyMember &&other) {
        Base::operator=(std::move(other));
        return *this;
    }
    DummyMember &operator=(std::nullptr_t) {
        Base::operator=(nullptr);
        return *this;
    }
    DummyMember &operator=(Base &&other) {
        Base::operator=(std::move(other));
        return *this;
    }
    DummyMember &operator=(const Base &other) {
        Base::operator=(other);
        return *this;
    }
};

}// namespace rc
