#pragma once
#include <memory>
#include <memory_resource>
#include <functional>
#include <utility>
#define GC_ASSERT(x, msg)                               \
    do {                                                \
        if (!(x)) [[unlikely]] {                        \
            std::printf("Assertion failed: %s\n", msg); \
            std::abort();                               \
        }                                               \
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
using TracingCallback = std::function<void(const GcObjectContainer *)>;
template<class T>
struct apply_trace {
    void operator()(const TracingCallback &, const T &) const {}
};
struct Tracer {
    TracingCallback &cb;
    template<class... Ts>
    void operator()(Ts &&...ts) const {
        (apply_trace<std::decay_t<Ts>>{}(cb, std::forward<Ts>(ts)), ...);
    }
};

class Traceable {
public:
    virtual void trace(const Tracer &) const = 0;
    ~Traceable() {}
};
namespace color {
constexpr uint8_t WHITE = 0;
constexpr uint8_t GRAY = 1;
constexpr uint8_t BLACK = 2;
}// namespace color
class GcObjectContainer {
    // we make this a base class so that we can handle classes that are not traceable
    template<class T>
    friend class Local;
protected:
    // 0th bit: is_root
    // 1-2nd bit: color
    // highest bit: set to 1 if the object is not collected
    mutable uint8_t tag = 0;
    friend class GcHeap;
    mutable GcObjectContainer *next_ = nullptr;
    void set_root(bool value) const {
        if (value) {
            detail::set_bit<0>(tag);
        } else {
            detail::clear_bit<0>(tag);
        }
    }
    void set_color(uint8_t value) const {
        tag &= 0b11111001;
        tag |= (value << 1);
    }
    void set_alive(bool value) const {
        if (value) {
            detail::set_bit<7>(tag);
        } else {
            detail::clear_bit<7>(tag);
        }
    }
public:
    bool is_root() const {
        return detail::get_bit<0>(tag);
    }

    uint8_t color() const {
        return (tag >> 1) & 0b11;
    }

    bool is_alive() const {
        return detail::get_bit<7>(tag);
    }
    virtual ~GcObjectContainer() {}
    virtual const Traceable *as_tracable() const { return nullptr; }
};
template<class T>
concept is_traceable = std::is_base_of<Traceable, T>::value;
template<class T>
class ContainerImpl : public GcObjectContainer {
    T object_;
    template<class T>
    friend class Local;
    template<class T>
    friend class GcPtr;
public:
    explicit ContainerImpl(T &&object) : object_(std::move(object)) {}
    ~ContainerImpl() override {}
    const Traceable *as_tracable() const override {
        if constexpr (is_traceable<T>) {
            return static_cast<const Traceable *>(&object_);
        } else {
            return nullptr;
        }
    }
};

class GcHeap {
    std::pmr::unsynchronized_pool_resource pool_;
    std::pmr::polymorphic_allocator<void> alloc_;
    GcObjectContainer *head_ = nullptr;
    // free an object. *Be careful*, this function does not update the head pointer or the next pointer of the object
    void free_object(GcObjectContainer *ptr) {
        ptr->set_alive(false);
        ptr->~GcObjectContainer();
        alloc_.deallocate_object(ptr);
    }
public:
    GcHeap() : pool_(), alloc_(&pool_) {}
    template<class T, class... Args>
    ContainerImpl<T> *_new_object(Args &&...args) {
        auto ptr = alloc_.new_object<ContainerImpl<T>>(T(std::forward<Args>(args)...));
        if constexpr (is_debug) {
            std::printf("Allocated object %p\n", static_cast<void *>(ptr));
        }
        ptr->set_alive(true);
        if (head_ == nullptr) {
            head_ = ptr;
        } else {
            ptr->next_ = head_;
            head_ = ptr;
        }
        return ptr;
    }
    void collect();
    ~GcHeap() {
        collect();
        GC_ASSERT(head_ == nullptr, "Memory leak detected");
    }
};
GcHeap &get_heap();
template<class T>
class GcPtr {
    template<class T>
    friend class Local;
    ContainerImpl<T> *container_;
    template<class T, class... Args>
    friend GcPtr<T> make_gc_ptr(Args &&...args);
    template<class U>
    friend struct apply_trace;

    explicit GcPtr(ContainerImpl<T> *container_) : container_(container_) {}

public:
    GcPtr() : container_(nullptr) {}
    T *operator->() const {
        check_alive();
        return &container_->object_;
    }
    T &operator*() const {
        check_alive();
        return container_->object_;
    }
    GcObjectContainer *gc_object_container() const {
        return container_;
    }
    void check_alive() const {
        if constexpr (is_debug) {
            if (!container_) {
                return;
            }
            if (!container_->is_alive()) [[unlikely]] {
                std::printf("Dangling pointer detected\n");
                std::abort();
            }
        }
    }
};
template<class T>
struct apply_trace<GcPtr<T>> {
    void operator()(const TracingCallback &cb, const GcPtr<T> &ptr) {
        if (ptr.container_ == nullptr) {
            return;
        }
        cb(ptr.container_);
    }
};

template<class T, class... Args>
GcPtr<T> make_gc_ptr(Args &&...args) {
    auto &heap = get_heap();
    auto gc_ptr = GcPtr<T>{heap._new_object<T>(std::forward<Args>(args)...)};
    return gc_ptr;
}
template<class T>
class Local {
    GcPtr<T> ptr_;
    bool _is_root = false;
public:
    Local(GcPtr<T> ptr) : ptr_(ptr), _is_root(ptr.container_->is_root()) {
        if (!_is_root) {
            ptr.container_->set_root(true);
        }
    }
    // A local handle to a gc object
    ~Local() {
        if (!_is_root) {
            ptr_.container_->set_root(false);
        }
    }
    T *operator->() const {
        return ptr_.operator->();
    }
    T &operator*() const {
        return ptr_.operator*();
    }
};
template<class T>
class Member {
    GcPtr<T> ptr_;

public:
};
}// namespace gc