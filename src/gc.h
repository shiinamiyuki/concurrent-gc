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

namespace color {
constexpr uint8_t WHITE = 0;
constexpr uint8_t GRAY = 1;
constexpr uint8_t BLACK = 2;
}// namespace color

struct RootSet {
    std::list<const GcObjectContainer *> roots;
    using Node = std::list<const GcObjectContainer *>::iterator;
    Node add(const GcObjectContainer *ptr) {
        return roots.insert(roots.end(), ptr);
    }
    void remove(Node node) {
        roots.erase(node);
    }
    auto begin() {
        return roots.begin();
    }
    auto end() {
        return roots.end();
    }
    size_t size() const {
        return roots.size();
    }
};

class GcObjectContainer {
    // we make this a base class so that we can handle classes that are not traceable
    template<class T>
    friend class Local;
    template<class T>
    friend class GcPtr;
protected:
    friend class GcHeap;
    mutable uint8_t color_ = color::WHITE;
    mutable bool alive = true;
    mutable uint16_t root_ref_count = 0;
    mutable RootSet::Node root_node = {};
    mutable GcObjectContainer *next_ = nullptr;

    void set_alive(bool value) const {
        alive = value;
    }
    void set_color(uint8_t value) const {
        color_ = value;
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
    uint8_t color() const {
        return color_;
    }
    bool is_alive() const {
        return alive;
    }
    virtual ~GcObjectContainer() {}
    virtual const Traceable *as_tracable() const {
        return nullptr;
    }
    virtual size_t object_size() const = 0;
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
template<class Self>
class GarbageCollected : public Traceable {
public:
    size_t object_size() const override {
        return sizeof(Self);
    }
};
enum class GcMode : uint8_t {
    STOP_THE_WORLD,
    INCREMENTAL,
    CONCURRENT
};
struct WorkList {
    std::deque<const GcObjectContainer *> list;
    void append(const GcObjectContainer *ptr) {
        list.push_back(ptr);
    }
    const GcObjectContainer *pop() {
        GC_ASSERT(!list.empty(), "Work list should not be empty");
        auto ptr = list.front();
        list.pop_front();
        return ptr;
    }
    bool empty() const {
        return list.empty();
    }
};

struct GcOption {
    GcMode mode = GcMode::INCREMENTAL;
    size_t max_heap_size = 1024 * 1024 * 1024;
    double gc_threshold = 0.5;// when should a gc be triggered
};
class GcHeap {
    friend struct TracingContext;
    template<class T>
    friend class Member;
    template<class T>
    friend class Local;
    GcMode mode_ = GcMode::INCREMENTAL;
    size_t allocation_size_ = 0;
    size_t max_heap_size_ = 0;
    double gc_threshold_ = 0.5;
    std::pmr::unsynchronized_pool_resource pool_;
    std::pmr::polymorphic_allocator<void> alloc_;
    GcObjectContainer *head_ = nullptr;
    RootSet root_set_;
    enum class State {
        IDLE,
        MARKING,
        SWEEPING
    } state = State::IDLE;
    // free an object. *Be careful*, this function does not update the head pointer or the next pointer of the object
    void free_object(GcObjectContainer *ptr) {
        if constexpr (is_debug) {
            std::printf("freeing object %p, size=%lld, %lld/%lldB used\n", static_cast<void *>(ptr), ptr->object_size(), allocation_size_, max_heap_size_);
        }
        allocation_size_ -= ptr->object_size();
        ptr->set_alive(false);
        ptr->~GcObjectContainer();
        alloc_.deallocate_object(ptr);
    }
    WorkList work_list;

    void shade(const GcObjectContainer *ptr) {
        if (!ptr || ptr->color() != color::WHITE)
            return;
        ptr->set_color(color::GRAY);
        if (ptr->as_tracable()) {
            add_to_working_list(ptr);
        }
    }
    void scan(const GcObjectContainer *ptr) {
        if (!ptr) {
            return;
        }
        GC_ASSERT(ptr->color() == color::GRAY || ptr->is_root(), "Object should be gray");
        auto ctx = TracingContext{*this};
        if (auto traceable = ptr->as_tracable()) {
            traceable->trace(Tracer{ctx});
        }
        ptr->set_color(color::BLACK);
    }
    const GcObjectContainer *pop_from_working_list() {
        return work_list.pop();
    }
    struct gc_ctor_token_t {};
    void do_incremental(size_t inc_size) {
        GC_ASSERT(state != State::SWEEPING, "State should not be sweeping");
        if (state == State::MARKING) {
            if constexpr (is_debug) {
                std::printf("%lld items in work list\n", work_list.list.size());
            }
            for (int i = 0; i < 10; i++) {
                if (!mark_some()) {
                    sweep();
                    return;
                }
            }
            return;
        }
        GC_ASSERT(work_list.empty(), "Work list should be empty");
        bool threshold_condition = allocation_size_ + inc_size > max_heap_size_ * gc_threshold_;
        if constexpr (is_debug) {
            std::printf("allocation_size_ = %lld, max_heap_size_ = %lld, inc_size = %lld\n", allocation_size_, max_heap_size_, inc_size);
            std::printf("threshold_condition = %d\n", threshold_condition);
        }
        if (allocation_size_ + inc_size > max_heap_size_) {
            state = State::MARKING;
            while (mark_some()) {}
            sweep();
            return;
        }
        if (threshold_condition) {
            if constexpr (is_debug) {
                std::printf("threshold condition\n");
            }
            scan_roots();
        }
    }
    struct gc_memory_resource : std::pmr::memory_resource {
        GcHeap *heap;
        void *do_allocate(std::size_t bytes, std::size_t alignment) override {
            heap->on_allocation(bytes);
            heap->allocation_size_ += bytes;
            return heap->pool_.allocate(bytes, alignment);
        }
        void do_deallocate(void *p,
                           std::size_t bytes,
                           std::size_t alignment) override {
            heap->allocation_size_ -= bytes;
            heap->pool_.deallocate(p, bytes, alignment);
        }
        bool do_is_equal(const std::pmr::memory_resource &other) const noexcept override {
            return this == &other;
        }
    };
    gc_memory_resource gc_memory_resource_;/// making gc aware of non-traceable memory usage

    void on_allocation(size_t inc_size) {
        if (mode_ != GcMode::STOP_THE_WORLD) {
            do_incremental(inc_size);
        } else {
            if (allocation_size_ + inc_size > max_heap_size_) {
                collect();
            }
        }
    }
public:
    std::pmr::memory_resource *memory_resource() {
        return &gc_memory_resource_;
    }
    RootSet &root_set() {
        return root_set_;
    }
    GcHeap(GcOption option, gc_ctor_token_t) : pool_(), alloc_(&pool_) {
        mode_ = option.mode;
        max_heap_size_ = option.max_heap_size;
    }
    GcHeap(const GcHeap &) = delete;
    GcHeap &operator=(const GcHeap &) = delete;
    GcHeap(GcHeap &&) = delete;
    GcHeap &operator=(GcHeap &&) = delete;
    GcMode mode() const {
        return mode_;
    }
    static void init(GcOption option = {});
    template<class T, class... Args>
    auto *_new_object(Args &&...args) {
        on_allocation(sizeof(T));
        GC_ASSERT(allocation_size_ + sizeof(T) <= max_heap_size_, "Out of memory");
        auto ptr = alloc_.new_object<T>(std::forward<Args>(args)...);

        if constexpr (is_debug) {
            std::printf("Allocated object %p, %lld/%lldB used\n", static_cast<void *>(ptr), allocation_size_, max_heap_size_);
        }
        ptr->set_alive(true);
        if (mode() != GcMode::STOP_THE_WORLD) {
            if (state == State::MARKING) {
                shade(ptr);
            }
        }
        if (head_ == nullptr) {
            head_ = ptr;
        } else {
            ptr->next_ = head_;
            head_ = ptr;
        }
        allocation_size_ += sizeof(T);
        return ptr;
    }
    void collect();
    void sweep();
    void scan_roots();
    bool mark_some();
    void add_to_working_list(const GcObjectContainer *ptr) {
        work_list.append(ptr);
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
struct Adaptor : Traceable, T {
    using T::T;
    void trace(const Tracer &) const override {}
    size_t object_size() const override {
        return sizeof(Adaptor<T>);
    }
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
    RootSet::Node &root_node() const {
        static_assert(std::is_base_of_v<GcObjectContainer, T>, "T should be a subclass of GcObjectContainer");
        return container_->root_node;
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

/// @brief an on stack handle to a gc object
template<class T>
class Local {
    GcPtr<T> ptr_;
    void inc() {
        if (ptr_.gc_object_container()) {
            ptr_.gc_object_container()->inc_root_ref_count();
            if (ptr_.gc_object_container()->root_ref_count == 1) {
                // become a new root, add to the root set
                auto node = get_heap().root_set().add(ptr_.gc_object_container());
                ptr_.root_node() = node;
            }
        }
    }
    void dec() {
        if (ptr_.gc_object_container()) {
            ptr_.gc_object_container()->dec_root_ref_count();
            if (ptr_.gc_object_container()->root_ref_count == 0) {
                // remove from the root set
                get_heap().root_set().remove(ptr_.root_node());
            }
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
    template<class U>
    friend class GcArray;
    GcPtr<T> ptr_;
    GcObjectContainer *parent_;
    Member(GcPtr<T> ptr, GcObjectContainer *parent) : ptr_(ptr), parent_(parent) {}

    void update(GcPtr<T> ptr) {
        if (ptr_ == ptr) [[unlikely]] {
            return;
        }
        ptr_ = ptr;
        if (ptr.gc_object_container() == nullptr) {
            return;
        }

        auto &heap = get_heap();
        if (heap.mode() != GcMode::STOP_THE_WORLD && parent_->color() == color::BLACK) {
            if constexpr (is_debug) {
                std::printf("write barrier\n");
            }
            heap.shade(ptr.gc_object_container());
        }
    }
public:
    template<is_traceable U>
    explicit Member(U *parent) : ptr_(), parent_(parent) {}
    Member(Member &&) = delete;
    Member(const Member &) = delete;
    Member &operator=(std::nullptr_t) {
        ptr_.reset();
        return *this;
    }
    Member &operator=(Member<T> &&) = delete;
    Member &operator=(const Member<T> &other) {
        GcPtr<T> ptr = other.ptr_;
        update(ptr);
        return *this;
    }

    Member &operator=(const GcPtr<T> &ptr) {
        update(ptr);
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
    const GcObjectContainer *parent() const {
        return parent_;
    }
};
template<class T>
struct apply_trace<Member<T>> {
    void operator()(TracingContext &ctx, const Member<T> &ptr) {
        apply_trace<GcPtr<T>>{}(ctx, ptr.ptr_);
    }
};
template<class T>
class GcArray : public GarbageCollected<GcArray<T>> {
    std::pmr::vector<Member<T>> data_;
public:
    GcArray() : data_(get_heap().memory_resource()) {}
    void push_back(GcPtr<T> ptr) {
        data_.emplace_back(ptr, this);
    }
    void trace(const Tracer &tr) const override {
        for (auto &ptr : data_) {
            tr(ptr);
        }
    }
    Member<T> &operator[](size_t idx) const {
        return data_[idx];
    }
    size_t size() const {
        return data_.size();
    }
    bool empty() const {
        return data_.empty();
    }
    void pop_back() {
        data_.pop_back();
    }
};
}// namespace gc