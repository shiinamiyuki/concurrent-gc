#pragma once
#include <memory>
#include <memory_resource>
#include <functional>
#include <utility>
#include <deque>
#include <unordered_set>
#include <stacktrace>
#include <iostream>
#include <print>

#define GC_ASSERT(x, msg)                                    \
    do {                                                     \
        if (!(x)) [[unlikely]] {                             \
            std::cerr << "Assertion failed: " << msg << "\n" \
                      << std::stacktrace::current()          \
                      << std::endl;                          \
            std::abort();                                    \
        }                                                    \
    } while (0)
namespace gc {
#ifdef DEBUG
constexpr bool is_debug = true;
#else
constexpr bool is_debug = false;
#endif
constexpr bool verbose_output = true;
namespace detail {
// template<class T>
// T *encode_pointer(T *ptr, uint16_t tag) {
//     return reinterpret_cast<T *>(reinterpret_cast<uintptr_t>(ptr) | (tag << 48));
// }
// template<class T>
// T *decode_pointer(T *ptr) {
//     return reinterpret_cast<T *>(reinterpret_cast<uintptr_t>(ptr) & 0x0000FFFFFFFFFFFF);
// }
// template<class T>
// uint16_t get_pointer_tag(T *ptr) {
//     return static_cast<uint16_t>(reinterpret_cast<uintptr_t>(ptr) >> 48);
// }
template<size_t Idx, typename I>
constexpr bool get_bit(I bitmap) {
    return (bitmap >> Idx) & 1;
}
template<size_t Idx, typename I>
void set_bit(I &bitmap) {
    bitmap |= (1 << Idx);
}
template<size_t Idx, typename I>
void clear_bit(I &bitmap) {
    bitmap &= ~(1 << Idx);
}
}// namespace detail
class GcHeap;
class GcObjectContainer;
struct TracingContext {
    GcHeap &heap;
    explicit TracingContext(GcHeap &heap) : heap(heap) {}
    void shade(const GcObjectContainer *ptr) const noexcept;
};
template<class T>
struct apply_trace {
    void operator()(TracingContext &, const T &) const;
};
struct Tracer {
    TracingContext &cb;
    template<class... Ts>
    void operator()(Ts &&...ts) const {
        (apply_trace<std::decay_t<Ts>>{}(cb, std::forward<Ts>(ts)), ...);
    }
};
class Traceable;

// namespace color {
// constexpr uint8_t WHITE = 0;
// constexpr uint8_t GRAY = 1;
// constexpr uint8_t BLACK = 2;
// }// namespace color
class GcObjectContainer {
    // we make this a base class so that we can handle classes that are not traceable
    template<class T>
    friend class Local;
protected:
    friend class GcHeap;
    // 0th bit: is_marked
    // highest bit: set to 1 if the object is not collected
    mutable uint8_t tag = 0;
    mutable uint16_t root_ref_count = 0;
    mutable GcObjectContainer *next_ = nullptr;

    void set_alive(bool value) const {
        if (value) {
            detail::set_bit<7>(tag);
        } else {
            detail::clear_bit<7>(tag);
        }
    }
    void set_marked(bool value) const {
        if (value) {
            detail::set_bit<0>(tag);
        } else {
            detail::clear_bit<0>(tag);
        }
    }
    void inc_root_ref_count() const {
        root_ref_count++;
    }
    void dec_root_ref_count() const {
        root_ref_count--;
    }
public:
    bool is_root() const {
        return root_ref_count > 0;
    }
    bool is_marked() const {
        return detail::get_bit<0>(tag);
    }

    bool is_alive() const {
        return detail::get_bit<7>(tag);
    }
    virtual ~GcObjectContainer() {}
    virtual const Traceable *as_tracable() const {
        return nullptr;
    }
};
template<class T>
concept is_traceable = std::is_base_of<Traceable, T>::value;
class Traceable : public GcObjectContainer {
public:
    virtual void trace(const Tracer &) const = 0;
    const Traceable *as_tracable() const override {
        return this;
    }
    ~Traceable() {}
};
template<typename T>
class FallbackContainer : public GcObjectContainer {
    static_assert(!is_traceable<T>, "This should only be used for non-traceable objects");
    T object_;
    template<class T>
    friend class Local;
    template<class T>
    friend class GcPtr;
public:
    explicit FallbackContainer(T &&object) : object_(std::move(object)) {}
    ~FallbackContainer() override {}
};
enum class GcMode : uint8_t {
    STOP_THE_WORLD,
    INCREMENTAL,
    CONCURRENT
};
class GcHeap {
    friend struct TracingContext;
    template<class T>
    friend class Member;
    template<class T>
    friend class Local;
    GcMode mode_ = GcMode::INCREMENTAL;
    std::pmr::unsynchronized_pool_resource pool_;
    std::pmr::polymorphic_allocator<void> alloc_;
    GcObjectContainer *head_ = nullptr;
    // free an object. *Be careful*, this function does not update the head pointer or the next pointer of the object
    void free_object(GcObjectContainer *ptr) {
        ptr->set_alive(false);
        ptr->~GcObjectContainer();
        alloc_.deallocate_object(ptr);
    }
    std::unordered_set<const GcObjectContainer *> work_list;

    void shade(const GcObjectContainer *ptr) {
        if (!ptr || ptr->is_marked()) {
            return;
        }
        ptr->set_marked(true);
        if (ptr->as_tracable()) {
            add_to_working_list(ptr);
        }
    }
    void scan(const GcObjectContainer *ptr) {
        if (!ptr) {
            return;
        }
        auto ctx = TracingContext{*this};
        if (auto traceable = ptr->as_tracable()) {
            traceable->trace(Tracer{ctx});
        }
    }
    const GcObjectContainer *pop_from_working_list() {
        auto ptr = *work_list.begin();
        work_list.erase(work_list.begin());
        return ptr;
    }
public:
    bool is_black(const GcObjectContainer *ptr) const {
        return ptr->is_marked() && !is_grey(ptr);
    }
    bool is_grey(const GcObjectContainer *ptr) const {
        return work_list.contains(ptr);
    }
    bool is_white(const GcObjectContainer *ptr) const {
        return !ptr->is_marked();
    }
    GcMode mode() const {
        return mode_;
    }
    GcHeap() : pool_(), alloc_(&pool_) {}
    template<class T, class... Args>
    auto *_new_object(Args &&...args) {
        using object_type = std::conditional_t<is_traceable<T>, T, FallbackContainer<T>>;
        // auto ptr = alloc_.new_object<ContainerImpl<T>>(T(std::forward<Args>(args)...));
        object_type *ptr{nullptr};
        if constexpr (is_traceable<T>) {
            ptr = alloc_.new_object<T>(std::forward<Args>(args)...);
        } else {
            ptr = alloc_.new_object<FallbackContainer<T>>(T(std::forward<Args>(args)...));
        }
        if constexpr (is_debug) {
            std::printf("Allocated object %p\n", static_cast<void *>(ptr));
        }
        ptr->set_alive(true);
        if (mode() != GcMode::STOP_THE_WORLD) {
            shade(ptr);
        }
        if (head_ == nullptr) {
            head_ = ptr;
        } else {
            ptr->next_ = head_;
            head_ = ptr;
        }
        return ptr;
    }
    void collect();
    void sweep();
    void scan_roots();
    bool mark_some();
    void add_to_working_list(const GcObjectContainer *ptr) {
        work_list.insert(ptr);
    }
    ~GcHeap() {
        sweep();
        GC_ASSERT(head_ == nullptr, "Memory leak detected");
    }
};
GcHeap &get_heap();
namespace detail {
inline bool check_alive(const GcObjectContainer *ptr) {
    if constexpr (is_debug) {
        if (!ptr) {
            return true;
        }
        GC_ASSERT(ptr->is_alive(), "Dangling pointer detected");
    }
    return true;
}
}// namespace detail
template<typename T>
    requires(!is_traceable<T>)
struct Adaptor : T, Traceable {
    using T::T;
    void trace(const Tracer &) const override {}
};
/// @brief GcPtr is an reference to a gc object
/// @brief GcPtr should never be stored in any object, but only for passing around
template<class T>
class GcPtr {
    template<class T>
    friend class Local;
    template<class T>
    friend class Member;
    // here is the traceable implementation
    T *container_;
    template<class U>
    friend struct apply_trace;
    explicit GcPtr(T *container_) : container_(container_) {}
    GcPtr() : container_(nullptr) {}
    void reset() {
        container_ = nullptr;
    }
public:
    T *operator->() const {
        detail::check_alive(container_);
        return static_cast<T *>(container_);
    }
    T &operator*() const {
        detail::check_alive(container_);
        return *static_cast<T *>(container_);
    }
    const GcObjectContainer *gc_object_container() const {
        return container_;
    }
    bool operator==(const GcPtr<T> &other) const {
        return container_ == other.container_;
    }
};
template<class T>
struct apply_trace<GcPtr<T>> {
    void operator()(TracingContext &ctx, const GcPtr<T> &ptr) {
        if (ptr.container_ == nullptr) {
            return;
        }
        ctx.shade(ptr.gc_object_container());
    }
};
template<class T>
struct apply_trace<Member<T>> {
    void operator()(TracingContext &ctx, const Member<T> &ptr) {
        apply_trace<GcPtr<T>>{}(ctx, ptr.ptr_);
    }
};
/// @brief an on stack handle to a gc object
template<class T>
class Local {
    GcPtr<T> ptr_;
    void inc() {
        if (ptr_.gc_object_container()) {
            ptr_.gc_object_container()->inc_root_ref_count();
            if (ptr_.gc_object_container()->root_ref_count == 1) {
                // become a new root, shade it
                get_heap().shade(ptr_.gc_object_container());
            }
        }
    }
    void dec() {
        if (ptr_.gc_object_container()) {
            ptr_.gc_object_container()->dec_root_ref_count();
        }
    }
public:
    Local() : ptr_() {}
    Local(const Local &other) : ptr_(other.ptr_) {
        inc();
    }
    Local(Local &&other) noexcept : ptr_(other.ptr_) {
        other.ptr_.reset();
    }
    explicit Local(GcPtr<T> ptr) : ptr_(ptr) {
        inc();
    }
    template<class... Args>
    static Local make(Args &&...args) {
        auto &heap = get_heap();
        auto gc_ptr = GcPtr<T>{heap._new_object<T>(std::forward<Args>(args)...)};
        return Local(gc_ptr);
    }
    // A local handle to a gc object
    ~Local() {
        dec();
    }
    T *operator->() const {
        return ptr_.operator->();
    }
    T &operator*() const {
        return ptr_.operator*();
    }
    operator GcPtr<T>() const {
        return ptr_;
    }
    Local &operator=(std::nullptr_t) {
        dec();
        ptr_.reset();
        return *this;
    }
    Local &operator=(Local &&other) noexcept {
        dec();
        ptr_ = other.ptr_;
        other.ptr_.reset();
        return *this;
    }
    Local &operator=(const Local &other) {
        dec();
        ptr_ = other.ptr_;
        inc();
        return *this;
    }
    Local &operator=(const GcPtr<T> &ptr) {
        dec();
        ptr_ = ptr;
        inc();
        return *this;
    }
    const GcObjectContainer *gc_object_container() const {
        return ptr_.gc_object_container();
    }
};
template<class T>
class Member {
    template<class U>
    friend struct apply_trace;
    GcPtr<T> ptr_;
    GcObjectContainer *parent_;
    Member(GcPtr<T> ptr, GcObjectContainer *parent) : ptr_(ptr), parent_(parent) {}
public:
    template<is_traceable U>
    explicit Member(U *parent) : ptr_(), parent_(parent) {}
    Member &operator=(std::nullptr_t) {
        ptr_.reset();
        return *this;
    }
    // Dijsktra's write barrier
    Member &operator=(const GcPtr<T> &ptr) {
        if (ptr_ == ptr) [[unlikely]] {
            return *this;
        }
        ptr_ = ptr;
        if (ptr.gc_object_container() == nullptr) {
            return *this;
        }

        auto &heap = get_heap();
        if (heap.mode() != GcMode::STOP_THE_WORLD && heap.is_black(parent_)) {
            if constexpr (is_debug) {
                std::printf("write barrier\n");
            }
            heap.shade(ptr.gc_object_container());
        }
        return *this;
    }
    T *operator->() const {
        return ptr_.operator->();
    }
    T &operator*() const {
        return ptr_.operator*();
    }
    const GcObjectContainer *gc_object_container() const {
        return ptr_.gc_object_container();
    }
};
}// namespace gc