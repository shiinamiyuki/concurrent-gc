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
#include <emmintrin.h>

#define GC_ASSERT(x, msg)                                    \
    do {                                                     \
        if (!(x)) [[unlikely]] {                             \
            std::cerr << "Assertion failed: at "             \
                      << __FILE__ << ":" << __LINE__ << "\n" \
                      << msg << "\n"                         \
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
struct spin_lock {// https://rigtorp.se/spinlock/
    std::atomic<bool> lock_ = false;
    void lock() {
        for (;;) {
            if (!lock_.exchange(true, std::memory_order_acquire)) {
                break;
            }
            while (lock_.load(std::memory_order_relaxed)) {
                _mm_pause();
            }
        }
    }
    void unlock() {
        lock_.store(false, std::memory_order_release);
    }
};
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
    virtual size_t object_alignment() const = 0;
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

#define GC_CLASS(...)                                     \
    void trace(const gc::Tracer &tracer) const override { \
        tracer(__VA_ARGS__);                              \
    }                                                     \
    size_t object_size() const override {                 \
        return sizeof(*this);                             \
    }                                                     \
    size_t object_alignment() const override {            \
        return alignof(std::decay_t<decltype(*this)>);    \
    }

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
    detail::spin_lock lock_;
    enum class State {
        IDLE,
        MARKING,
        SWEEPING
    } state = State::IDLE;
    WorkList work_list;
    // free an object. *Be careful*, this function does not update the head pointer or the next pointer of the object
    void free_object(GcObjectContainer *ptr) {
        if constexpr (is_debug) {
            std::printf("freeing object %p, size=%lld, %lld/%lldB used\n", static_cast<void *>(ptr), ptr->object_size(), allocation_size_, max_heap_size_);
        }
        allocation_size_ -= ptr->object_size();
        ptr->set_alive(false);
        auto size = ptr->object_size();
        auto align = ptr->object_alignment();
        ptr->~GcObjectContainer();
        alloc_.deallocate_bytes(ptr, size, align);
    }

    void shade(const GcObjectContainer *ptr) {
        if (state != State::MARKING) {
            return;
        }
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
            collect();
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
        gc_memory_resource(GcHeap *heap) : heap(heap) {}
        void *do_allocate(std::size_t bytes, std::size_t alignment) override {
            heap->prepare_allocation(bytes);
            heap->allocation_size_ += bytes;
            auto ptr = heap->pool_.allocate(bytes, alignment);
            if constexpr (is_debug) {
                std::printf("Allocating %p, %lld bytes via pmr, %lld/%lldB used\n", ptr, bytes, heap->allocation_size_, heap->max_heap_size_);
            }
            return ptr;
        }
        void do_deallocate(void *p,
                           std::size_t bytes,
                           std::size_t alignment) override {
            heap->allocation_size_ -= bytes;
            if constexpr (is_debug) {
                std::printf("Deallocating %p, %lld bytes via pmr, %lld/%lldB used\n", p, bytes, heap->allocation_size_, heap->max_heap_size_);
            }
            heap->pool_.deallocate(p, bytes, alignment);
        }
        bool do_is_equal(const std::pmr::memory_resource &other) const noexcept override {
            return this == &other;
        }
    };
    gc_memory_resource gc_memory_resource_;/// making gc aware of non-traceable memory usage

    void prepare_allocation(size_t inc_size) {
        with_lock([&] {
            if (mode_ != GcMode::STOP_THE_WORLD) {
                do_incremental(inc_size);
            } else {
                if (allocation_size_ + inc_size > max_heap_size_) {
                    collect();
                }
            }
        });
    }
public:
    template<class F>
    void with_lock(F &&f) noexcept {
        if (mode_ == GcMode::CONCURRENT) {
            std::unique_lock lock(lock_);
            f();
        } else {
            f();
        }
    }
    std::pmr::memory_resource *memory_resource() {
        return &gc_memory_resource_;
    }
    RootSet &root_set() {
        return root_set_;
    }
    GcHeap(GcOption option, gc_ctor_token_t) : pool_(), alloc_(&pool_), gc_memory_resource_{this} {
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
        requires std::constructible_from<T, Args...>
    auto *_new_object(Args &&...args) {
        prepare_allocation(sizeof(T));
        GC_ASSERT(allocation_size_ + sizeof(T) <= max_heap_size_, "Out of memory");

        // auto ptr = alloc_.new_object<T>(std::forward<Args>(args)...);
        auto ptr = alloc_.allocate_object<T>(1);
        new (ptr) T(std::forward<Args>(args)...);// avoid pmr intercepting the allocator

        if constexpr (is_debug) {
            std::printf("Allocated object %p, %lld/%lldB used\n", static_cast<void *>(ptr), allocation_size_, max_heap_size_);
        }
        ptr->set_alive(true);
        GC_ASSERT(sizeof(T) == ptr->object_size(), "size should be the same");
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
        collect();
        GC_ASSERT(head_ == nullptr, "Memory leak detected");
    }
    bool need_write_barrier() const {
        if (mode_ == GcMode::STOP_THE_WORLD) {
            return false;
        }
        // TODO: what about concurrent mode?
        return state == State::MARKING;
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

/// Thank you, C++
template<typename T>
    requires(!std::is_class_v<T>)
struct Boxed : Traceable {
    T value;
    Boxed() = default;
    Boxed(const T &value) : value(value) {}
    Boxed(T &&value) : value(std::move(value)) {}
    void trace(const Tracer &tracer) const override {}
    size_t object_size() const {
        return sizeof(Boxed<T>);
    }
    size_t object_alignment() const {
        return alignof(Boxed<T>);
    }
};
template<typename T>
    requires(!is_traceable<T>)
struct Adaptor : Traceable, T {
    // using T::T;
    // template<typename = void>
    //     requires std::is_move_constructible_v<T>
    // Adaptor(T &&t) : T(std::move(t)) {}
    // template<typename = void>
    //     requires std::is_copy_constructible_v<T>
    // Adaptor(const T &t) : T(t) {}
    template<class... Args>
        requires std::constructible_from<T, Args...>
    Adaptor(Args &&...args) : T(std::forward<Args>(args)...) {}
    void trace(const Tracer &) const override {}
    size_t object_size() const override {
        return sizeof(Adaptor<T>);
    }
    size_t object_alignment() const override {
        return alignof(Adaptor<T>);
    }
};
/// @brief GcPtr is an reference to a gc object
/// @brief GcPtr should never be stored in any object, but only for passing around
template<class T>
class GcPtr {
    template<class U>
    friend class Local;
    template<class U>
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
    bool operator==(std::nullptr_t) const {
        return container_ == nullptr;
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
                auto &heap = get_heap();
                get_heap().with_lock([&] {
                    auto node = heap.root_set().add(ptr_.gc_object_container());
                    if constexpr (is_debug) {
                        std::printf("adding root %p\n", static_cast<const void *>(ptr_.gc_object_container()));
                    }
                    heap.shade(ptr_.gc_object_container());
                    ptr_.root_node() = node;
                });
            }
        }
    }
    void dec() {
        if (ptr_.gc_object_container()) {
            ptr_.gc_object_container()->dec_root_ref_count();
            if (ptr_.gc_object_container()->root_ref_count == 0) {
                // remove from the root set
                if constexpr (is_debug) {
                    std::printf("removing root %p\n", static_cast<const void *>(ptr_.gc_object_container()));
                }
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
        requires std::constructible_from<T, Args...>
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
        if (ptr_ == other.ptr_) {
            return *this;
        }
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
    Member(GcObjectContainer *parent, GcPtr<T> ptr) : ptr_(ptr), parent_(parent) {}

    // Dijkstra's write barrier
    void update(GcPtr<T> ptr) {
        if (ptr_ == ptr) [[unlikely]] {
            return;
        }
        ptr_ = ptr;
        if (ptr.gc_object_container() == nullptr) {
            return;
        }

        auto &heap = get_heap();
        if (heap.need_write_barrier()) [[likely]] {
            heap.with_lock([&] {
                if (parent_->color() == color::BLACK) {
                    if constexpr (is_debug) {
                        std::printf("write barrier\n");
                    }
                    heap.shade(ptr.gc_object_container());
                }
            });
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
    bool operator==(const GcPtr<T> &other) const {
        return ptr_ == other;
    }
    bool operator==(const Member<T> &other) const {
        return ptr_ == other.ptr_;
    }
    bool operator==(std::nullptr_t) const {
        return ptr_ == nullptr;
    }
};
template<class T>
struct apply_trace<Member<T>> {
    void operator()(TracingContext &ctx, const Member<T> &ptr) {
        apply_trace<GcPtr<T>>{}(ctx, ptr.ptr_);
    }
};

/// @brief Fixed size array of gc objects
template<class T>
class GcArray : public Traceable {
    Member<T> *data_;
    size_t size_;
public:
    GcArray(size_t n) : data_(nullptr), size_(n) {
        auto &heap = get_heap();
        auto alloc = std::pmr::polymorphic_allocator(heap.memory_resource());
        data_ = alloc.allocate_object<Member<T>>(n);
        for (size_t i = 0; i < n; i++) {
            new (data_ + i) Member<T>(this);
        }
    }
    Member<T> &operator[](size_t idx) {
        return data_[idx];
    }
    Member<T> &operator[](size_t idx) const {
        return data_[idx];
    }
    size_t size() const {
        return size_;
    }
    bool empty() const {
        return size_ == 0;
    }
    void trace(const Tracer &tracer) const override {
        for (size_t i = 0; i < size_; i++) {
            tracer(data_[i]);
        }
    }
    size_t object_size() const override {
        return sizeof(*this);
    }
    size_t object_alignment() const override {
        return alignof(GcArray<T>);
    }
    ~GcArray() {
        auto &heap = get_heap();
        auto alloc = std::pmr::polymorphic_allocator(heap.memory_resource());
        for (size_t i = 0; i < size_; i++) {
            data_[i].~Member<T>();
        }
        alloc.deallocate_object(data_, size_);
    }
};
template<class T>
class GcVector : public Traceable {
    Member<GcArray<T>> data_;
    size_t size_;

    Local<GcArray<T>> alloc(size_t len) {
        return Local<GcArray<T>>::make(len);
    }
    void ensure_size(size_t new_size) {
        if (data_ == nullptr) {
            data_ = alloc(new_size);
            return;
        }
        if (new_size <= data_->size()) {
            return;
        }
        auto new_capacity = std::max(data_->size() * 2, new_size);
        Local<GcArray<T>> new_data = alloc(new_capacity);
        for (size_t i = 0; i < data_->size(); i++) {
            (*new_data)[i] = (*data_)[i];
        }
        data_ = new_data;
    }
public:
    size_t capacity() const {
        return data_.size();
    }
    GcVector() : data_(this), size_(0) {}
    void push_back(GcPtr<T> value) {
        ensure_size(size_ + 1);
        (*data_)[size_++] = value;
    }
    T &operator[](size_t idx) {
        return (*data_)[idx];
    }
    void pop_back() {
        size_--;
    }
    size_t size() const {
        return size_;
    }
    GC_CLASS(data_)
};

}// namespace gc