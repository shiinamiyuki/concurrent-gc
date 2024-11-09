#pragma once
#include <memory>
#include <memory_resource>
#include <functional>
#define GC_ASSERT(x, msg) do{ if(!(x)) { std::printf("Assertion failed: %s\n", msg); std::abort(); } } while(0)
namespace gc {
class GcHeap;
class GcObjectContainer;
using TracingCallback = std::function<void(const GcObjectContainer *)>;
enum Color {
    WHITE,
    GRAY,
    BLACK
};
class Traceable {
public:
    virtual void trace(const TracingCallback &) const = 0;
    ~Traceable() {}
};
class GcObjectContainer {
    template<class T>
    friend class Local;
protected:
    mutable bool is_root_ = false;
    mutable Color color_ = WHITE;
    friend class GcHeap;
    GcObjectContainer *next_ = nullptr;
public:
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
        ptr->~GcObjectContainer();
        alloc_.deallocate_object(ptr);
    }
public:
    GcHeap() : pool_(), alloc_(&pool_) {}
    template<class T, class... Args>
    ContainerImpl<T> *_new_object(Args &&...args) {
        auto ptr = alloc_.new_object<ContainerImpl<T>>(T(std::forward<Args>(args)...));
        if (head_ == nullptr) {
            head_ = ptr;
        } else {
            ptr->next_ = head_;
            head_ = ptr;
        }
        return ptr;
    }
    void collect();
};
GcHeap &get_heap();
template<class T>
class GcPtr {
    template<class T>
    friend class Local;
    ContainerImpl<T> *container_;
    template<class T, class... Args>
    friend GcPtr<T> make_gc_ptr(Args &&...args);
    explicit GcPtr(ContainerImpl<T> *container_) : container_(container_) {}
public:
    T *operator->() const {
        return &container_->object_;
    }
    T &operator*() const {
        return container_->object_;
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
    Local(GcPtr<T> ptr) : ptr_(ptr), _is_root(ptr.container_->is_root_) {
        if (!_is_root) {
            ptr.container_->is_root_ = true;
        }
    }
    // A local handle to a gc object
    ~Local() {
        if (!_is_root) {
            ptr_.container_->is_root_ = false;
        }
    }
    T *operator->() const {
        return &ptr_.container_->object_;
    }
    T &operator*() const {
        return ptr_.container_->object_;
    }
};


}// namespace gc