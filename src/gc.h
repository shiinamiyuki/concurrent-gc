#pragma once
#include <memory>
#include <memory_resource>
#include <functional>
#include <utility>
#include <deque>
#include <unordered_set>
#include <stacktrace>
#include <iostream>
#include <optional>
#include <thread>
#include <emmintrin.h>
#include <mutex>
#include <list>
#include <barrier>
#include <condition_variable>
#include <cstring>
#include "pmr-mimalloc.h"

static_assert(sizeof(size_t) == 8, "64-bit only");
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
extern bool enable_time_tracking;

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
inline void pause_thread() {
#if defined(__x86_64__) || defined(_M_X64) || defined(_WIN64)
    _mm_pause();
#else
    std::this_thread::yield();
#endif
}
// #ifdef DEBUG
// // mutex helps catching reentrant lock
// using spin_lock = std::mutex;
// #else
struct spin_lock {
    std::atomic<bool> lock_ = false;
    void lock() {
        for (;;) {
            if (!lock_.exchange(true, std::memory_order_acquire)) {
                break;
            }
            auto i = 1;
            while (lock_.load(std::memory_order_relaxed)) {
                for (auto j = 0; j < i; j++) {
                    detail::pause_thread();
                }
                if (i < 8192) {
                    i *= 2;
                }
            }
        }
    }
    void unlock() {
        lock_.store(false, std::memory_order_release);
    }
    template<class F>
    void wait(F &&f) {
        auto i = 1;
        while (f()) {
            unlock();
            for (auto j = 0; j < i; j++) {
                detail::pause_thread();
            }
            lock();
            if (i < 8192)
                i *= 2;
        }
    }
    bool try_lock() {
        return !lock_.exchange(true, std::memory_order_acquire);
    }
};
// #endif
template<class Mutex>
struct shared_lock {
    void lock_shared() {
        mutex.lock();
        // if(n_readers.load() > 1){
        //     std::printf("n_readers = %d\n", n_readers.load());
        // }
        mutex.wait([&] {
            return lock_exclusive.load(std::memory_order_relaxed);
        });
        n_readers.fetch_add(1, std::memory_order_relaxed);
        // if(n_readers.load() > 1){
        //     std::printf("n_readers = %d\n", n_readers.load());
        // }
        mutex.unlock();
    }
    void unlock_shared() {
        mutex.lock();
        n_readers.fetch_sub(1, std::memory_order_relaxed);
        mutex.unlock();
    }
    void lock() {
        mutex.lock();
        mutex.wait([&] {
            return lock_exclusive || n_readers > 0;
        });
        lock_exclusive = true;
        mutex.unlock();
    }
    void unlock() {
        mutex.lock();
        lock_exclusive = false;
        mutex.unlock();
    }
    template<class F>
    void wait(F &&f) {
        auto i = 1;
        while (f()) {
            unlock();
            for (auto j = 0; j < i; j++) {
                detail::pause_thread();
            }
            lock();
            if (i < 8192)
                i *= 2;
        }
    }
    template<class F>
    void wait_shared(F &&f) {
        auto i = 1;
        while (f()) {
            unlock_shared();
            for (auto j = 0; j < i; j++) {
                detail::pause_thread();
            }
            lock_shared();
            if (i < 8192)
                i *= 2;
        }
    }
    shared_lock() = default;
private:
    Mutex mutex;
    std::atomic<bool> lock_exclusive = false;
    std::atomic<int> n_readers = 0;
};
struct recursive_spinlock {
private:
    spin_lock mutex;
    std::thread::id owner;
    size_t count = 0;
public:
    void lock() {
        auto id = std::this_thread::get_id();
        if (id == owner) {
            count++;
            return;
        }
        mutex.lock();
        GC_ASSERT(count == 0, "Reentrant lock");

        owner = id;
        count = 1;
    }
    void unlock() {
        auto id = std::this_thread::get_id();
        GC_ASSERT(id == owner, "Unlocking lock not owned by this thread");
        GC_ASSERT(count > 0, "Unlocking unlocked lock");
        count--;
        if (count == 0) {
            owner = {};
            mutex.unlock();
        }
    }
    template<class F>
    void wait(F &&f) {
        auto i = 1;
        while (f()) {
            unlock();
            for (auto j = 0; j < i; j++) {
                detail::pause_thread();
            }
            lock();
            if (i < 8192)
                i *= 2;
        }
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
template<class T>
concept Lockable = requires(T lock) {
    lock.lock();
    lock.unlock();
};
struct emplace_t {};
template<Lockable Lock, class T>
class LockProtected {
    T data;
    std::optional<Lock> lock_;
public:

    LockProtected() = delete;
    LockProtected(bool enable) : data() {
        if (enable) {
            lock_.emplace();
        }
    }
    LockProtected(T data, bool enable) : data(std::move(data)) {
        if (enable) {
            lock_.emplace();
        }
    }
    template<class... Args>
    LockProtected(emplace_t, bool enable, Args &&...args) : data(std::forward<Args>(args)...) {
        if (enable) {
            lock_.emplace();
        }
    }
    auto &get_lock() {
        return lock_.value();
    }
    bool lock_enabled() const {
        return lock_.has_value();
    }
    template<class F>
        requires std::invocable<F, T &, Lock *>
    auto with(F &&f, bool no_lock = false, bool no_unlock = false) -> decltype(auto) {
        using R = std::invoke_result_t<F, T &, Lock *>;
        if (!no_lock && lock_.has_value()) {
            lock_->lock();
        }
        if constexpr (std::is_void_v<R>) {
            f(data, lock_.has_value() ? &lock_.value() : nullptr);
            if (lock_.has_value()) {
                if (!no_unlock) {
                    lock_->unlock();
                }
            }
            return;
        } else {
            auto result = f(data, lock_.has_value() ? &lock_.value() : nullptr);
            if (lock_.has_value()) {
                if (!no_unlock) {
                    lock_->unlock();
                }
            }
            return result;
        }
    }
    template<class F>
        requires std::invocable<F, T &, Lock *>
    auto with_shared(F &&f, bool no_lock = false, bool no_unlock = false) -> decltype(auto) {
        using R = std::invoke_result_t<F, T &, Lock *>;
        if (!no_lock && lock_.has_value()) {
            lock_->lock_shared();
        }
        if constexpr (std::is_void_v<R>) {
            f(data, lock_.has_value() ? &lock_.value() : nullptr);
            if (lock_.has_value()) {
                if (!no_unlock) {
                    lock_->unlock_shared();
                }
            }
            return;
        } else {
            auto result = f(data, lock_.has_value() ? &lock_.value() : nullptr);
            if (lock_.has_value()) {
                if (!no_unlock) {
                    lock_->unlock_shared();
                }
            }
            return result;
        }
    }
    template<class F>
        requires std::invocable<F, T &, Lock *>
    auto with_timed(F &&f, bool no_lock = false, bool no_unlock = false) -> decltype(auto) {
        auto lock_time = 0.0;
        using R = std::invoke_result_t<F, T &, Lock *>;
        if (!no_lock && lock_.has_value()) {
            if (enable_time_tracking) {
                auto t = std::chrono::steady_clock::now();
                lock_->lock();
                lock_time = (std::chrono::steady_clock::now() - t).count() * 1e-9;
            } else {
                lock_->lock();
            }
        }
        if constexpr (std::is_void_v<R>) {
            f(data, lock_.has_value() ? &lock_.value() : nullptr);
            if (!no_unlock && lock_.has_value()) {
                lock_->unlock();
            }
            return lock_time;
        } else {
            auto result = f(data, lock_.has_value() ? &lock_.value() : nullptr);
            if (!no_unlock && lock_.has_value()) {
                lock_->unlock();
            }
            return std::make_pair(result, lock_time);
        }
    }
    T &get() {
        return data;
    }
};
}// namespace detail
class GcHeap;
class GcObjectContainer;
struct TracingContext {
    GcHeap &heap;
    size_t pool_idx;
    explicit TracingContext(GcHeap &heap, size_t pool_idx) : heap(heap), pool_idx(pool_idx) {}
    void shade(const GcObjectContainer *ptr) const noexcept;
};
template<class T>
struct apply_trace {
    void operator()(TracingContext &, const T &) const {}
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
    mutable std::atomic<uint8_t> color_ = color::WHITE;
    mutable bool alive = true;
    uint8_t pool_idx_;
    mutable std::atomic<uint16_t> root_ref_count = 0;
    /// @brief if we were using rust, the below field might only take 8 bytes...
    mutable std::optional<RootSet::Node> root_node = {};
    mutable GcObjectContainer *next_ = nullptr;

    void set_alive(bool value) const {
        alive = value;
    }
    void set_color(uint8_t value) const {
        color_.store(value, std::memory_order_relaxed);
    }
    void inc_root_ref_count() const {
        root_ref_count.fetch_add(1, std::memory_order_relaxed);
    }
    void dec_root_ref_count() const {
        root_ref_count.fetch_sub(1, std::memory_order_relaxed);
    }
public:
    size_t pool_idx() const {
        return pool_idx_;
    }
    bool is_root() const {
        // return root_ref_count > 0;
        return root_node.has_value();
    }
    uint8_t color() const {
        return color_.load(std::memory_order_relaxed);
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
// this is just a dummy class to allow us to use the same interface as rc
template<class T>
class GarbageCollected : public Traceable {};

#define GC_CLASS(...)                                   \
    void trace(const gc::Tracer &tracer) const {        \
        tracer(__VA_ARGS__);                            \
    }                                                   \
    size_t object_size() const {                        \
        return sizeof(*this);                           \
    }                                                   \
    size_t object_alignment() const {                   \
        return alignof(std::decay_t<decltype(*this)>);  \
    }                                                   \
    auto gc_ptr_from_this() {                           \
        using SelfType = std::decay_t<decltype(*this)>; \
        return gc::GcPtr<SelfType>(this);               \
    }

enum class GcMode : uint8_t {
    STOP_THE_WORLD,
    INCREMENTAL,
    CONCURRENT
};
inline const char *to_string(GcMode mode) {
    switch (mode) {
        case GcMode::STOP_THE_WORLD:
            return "STOP_THE_WORLD";
        case GcMode::INCREMENTAL:
            return "INCREMENTAL";
        case GcMode::CONCURRENT:
            return "CONCURRENT";
    }
    return "UNKNOWN";
}
struct WorkList {
    // std::vector<const GcObjectContainer *> list;
    using list_t = detail::LockProtected<detail::spin_lock, std::deque<const GcObjectContainer *>>;
    std::vector<std::unique_ptr<list_t>> lists;
    void append(const GcObjectContainer *ptr) {
        // if (lists.size() > 1) {
        //     auto tid = std::hash<std::thread::id>{}(std::this_thread::get_id()) % lists.size();
        //     lists[tid]->with([&](auto &list, auto *lock) {
        //         list.push_back(ptr);
        //     });
        // } else {
        //     lists[0]->get().push_back(ptr);
        // }
        GC_ASSERT(lists.size() == 1, "Only one work list is supported");
        lists[0]->get().push_back(ptr);
    }
    size_t least_filled() const {
        size_t min = std::numeric_limits<size_t>::max();
        size_t idx = 0;
        for (size_t i = 0; i < lists.size(); i++) {
            auto &list = lists[i]->get();
            if (list.size() < min) {
                min = list.size();
                idx = i;
            }
        }
        return idx;
    }
    const GcObjectContainer *pop() {
        list_t *list = nullptr;
        GC_ASSERT(lists.size() == 1, "Only one work list is supported");
        list = lists[0].get();

        return list->with([&](auto &list, auto *lock) {
            GC_ASSERT(!list.empty(), "Work list should not be empty");
            auto ptr = list.front();
            list.pop_front();
            return ptr;
        });
    }
    // std::vector<const GcObjectContainer *> pop_n(size_t n) {
    //     std::vector<const GcObjectContainer *> result;
    //     list_t *list = nullptr;
    //     GC_ASSERT(lists.size() == 1, "Only one work list is supported");
    //     list = lists[0].get();

    //     return list->with([&](auto &list, auto *lock) {
    //         for (size_t i = 0; i < n; i++) {
    //             result.push_back(list.front());
    //             list.pop_front();
    //         }
    //         return result;
    //     });
    // }
    bool empty() const {
        bool empty = true;
        for (auto &list : lists) {
            empty &= list->get().empty();
        }
        return empty;
    }
    void clear() {
        for (auto &list : lists) {
            list->with([](auto &list, auto *lock) {
                list.clear();
            });
        }
    }
};

struct GcOption {
    GcMode mode = GcMode::INCREMENTAL;
    size_t max_heap_size = 1024 * 1024 * 1024;
    double gc_threshold = 0.8;// when should a gc be triggered
    bool _full_debug = false;
    std::optional<size_t> n_collector_threads = {};
};
// namespace detail {
// struct new_but_no_delete_memory_resouce : std::pmr::memory_resource {
//     std::pmr::memory_resource *inner;
//     new_but_no_delete_memory_resouce() : inner(std::pmr::new_delete_resource()) {}
//     void *do_allocate(std::size_t bytes, std::size_t alignment) override {
//         return inner->allocate(bytes, alignment);
//     }
//     void do_deallocate(void *p, std::size_t bytes, std::size_t alignment) override {
//         // do nothing
//     }
//     bool do_is_equal(const std::pmr::memory_resource &other) const noexcept override {
//         return this == &other;
//     }
// };
// }// namespace detail
struct StatsTracker {
    double mean = 0;
    double max = 0;
    double min = std::numeric_limits<double>::max();
    double m2 = 0;
    size_t count = 0;
    void update(double x) {
        max = std::max(max, x);
        min = std::min(min, x);
        count++;
        double delta = x - mean;
        mean += delta / count;
        double delta2 = x - mean;
        m2 += delta * delta2;
    }
    double variance() const {
        return m2 / count;
    }
    void print(const char *name) const {
        if (count > 1) {
            std::printf("%s: mean = %f, max = %f, min = %f, variance = %f\n", name, mean, max, min, variance());
        } else {
            std::printf("%s: mean = %f\n", name, mean);
        }
    }
};
struct GcStats {
    std::atomic<size_t> n_allocated = 0;
    std::atomic<size_t> n_collected = 0;
    std::atomic<size_t> n_collection_cycles = 0;
    size_t last_collected = 0;
    std::chrono::high_resolution_clock::time_point last_collect_time = std::chrono::high_resolution_clock::now();
    StatsTracker collection_time;
    StatsTracker ratio_collected;
    StatsTracker sweep_time;
    double incremental_time = 0;
    double wait_for_atomic_marking = 0;
    double time_waiting_for_pool = 0;
    double time_waiting_for_object_list = 0;
    double time_waiting_for_work_list = 0;
    double time_waiting_for_root_set = 0;
    void print() const {
        std::printf("GC stats\n");
        std::printf("n_allocated = %lld\n", n_allocated.load());
        std::printf("n_collection_cycles = %lld\n", n_collection_cycles.load());
        std::printf("mutator waiting for atomic marking = %f\n", wait_for_atomic_marking);
        std::printf("mutator waiting for pool = %f\n", time_waiting_for_pool);
        std::printf("mutator waiting for object list = %f\n", time_waiting_for_object_list);
        std::printf("mutator waiting for work list = %f\n", time_waiting_for_work_list);
        std::printf("mutator waiting for root set = %f\n", time_waiting_for_root_set);
        sweep_time.print("sweep_time");
        collection_time.print("collection_time");
        ratio_collected.print("ratio_collected");
    }
    void reset() {
        n_allocated = 0;
        n_collection_cycles = 0;
        incremental_time = 0;
        wait_for_atomic_marking = 0;
        time_waiting_for_pool = 0;
        time_waiting_for_object_list = 0;
        time_waiting_for_work_list = 0;
        time_waiting_for_root_set = 0;
        collection_time = {};
        ratio_collected = {};
        sweep_time = {};
    }
};
template<class F, typename R = std::invoke_result_t<F>>
auto time_function(F &&f) {
    if (enable_time_tracking) {
        if constexpr (std::is_same_v<R, void>) {
            auto start = std::chrono::high_resolution_clock::now();
            f();
            auto end = std::chrono::high_resolution_clock::now();
            double elapsed = static_cast<double>((end - start).count() * 1e-9);
            return elapsed;
        } else {
            auto start = std::chrono::high_resolution_clock::now();
            auto result = f();
            auto end = std::chrono::high_resolution_clock::now();
            double elapsed = static_cast<double>((end - start).count() * 1e-9);
            return std::make_pair(result, elapsed);
        }
    } else {
        if constexpr (std::is_same_v<R, void>) {
            f();
            return 0.0;
        } else {
            return std::make_pair(f(), 0.0);
        }
    }
}
struct ThreadPool {
    std::vector<std::thread> threads;
    std::mutex mutex;
    std::condition_variable has_work, has_result;
    std::atomic<bool> stop = false;
    std::optional<std::function<void(size_t)>> work{};
    std::barrier<> barrier;
public:
    ThreadPool(size_t n_threads = 4) : barrier(n_threads) {
        for (size_t i = 0; i < n_threads; i++) {
            threads.emplace_back([=, this] {
                while (!stop) {
                    std::unique_lock<std::mutex> lock(mutex);
                    has_work.wait(lock, [this] { return stop || work.has_value(); });
                    if (stop || !work.has_value()) {
                        return;
                    }
                    auto &f = work.value();
                    lock.unlock();
                    f(i);
                    // std::printf("thread %lld done\n", i);
                    barrier.arrive_and_wait();
                    if (i == 0) {
                        // std::printf("thread %lld notifying\n", i);
                        work.reset();
                        has_result.notify_all();
                    }
                    barrier.arrive_and_wait();
                }
            });
        }
    }
    template<class F>
        requires std::invocable<F, size_t>
    void dispatch(F &&f) {
        std::unique_lock<std::mutex> lock(mutex);
        work.emplace(std::forward<F>(f));
        has_work.notify_all();
        has_result.wait(lock);
        GC_ASSERT(!work.has_value(), "Work should be done");
    }
    ~ThreadPool() {
        stop = true;
        has_work.notify_all();
        for (auto &t : threads) {
            t.join();
        }
    }
};
// template<class T>
// struct tracking_memory_resource : std::pmr::memory_resource {
//     std::unique_ptr<std::pmr::memory_resource> inner;
//     T payload;
//     struct Metadata {
//         tracking_memory_resource *self;
//     };
//     tracking_memory_resource(T payload, std::unique_ptr<std::pmr::memory_resource> inner) : payload(std::move(payload)) {
//         inner = std::move(inner);
//     }
//     void *do_allocate(std::size_t bytes, std::size_t alignment) override {
//         auto actual_size = bytes + sizeof(Metadata);
//         auto ptr = reinterpret_cast<uint8_t *>(inner->allocate(actual_size, alignment));
//         auto metadata = new (ptr + bytes) Metadata{this};
//         return ptr;
//     }
//     void do_deallocate(void *p,
//                        std::size_t bytes,
//                        std::size_t alignment) override {
//         auto ptr = static_cast<uint8_t *>(p);
//         auto metadata = reinterpret_cast<Metadata *>(ptr + bytes);
//         GC_ASSERT(metadata->self == this, "Invalid metadata");
//         inner->deallocate(ptr, bytes + sizeof(Metadata), alignment);
//     }
//     bool do_is_equal(const std::pmr::memory_resource &other) const noexcept override {
//         return this == &other;
//     }
// };
extern thread_local std::optional<size_t> tl_pool_idx;
class GcHeap {
    friend struct TracingContext;
    template<class T>
    friend class Member;
    template<class T>
    friend class Local;
    enum class State {
        IDLE,
        MARKING,
        SWEEPING
    };
    enum class ConcurrentState {
        IDLE,
        REQUESTED,
        MARKING,
        ATOMIC_MARKING,
        SWEEPING
    };
    std::optional<ThreadPool> worker_pool_;
    struct Pool {
        std::atomic<size_t> allocation_size_ = 0;
        // std::unique_ptr<std::pmr::memory_resource> inner;
        using resouce_t = detail::LockProtected<detail::spin_lock, std::unique_ptr<std::pmr::memory_resource>>;
        std::vector<std::unique_ptr<resouce_t>> concurrent_resources;
        volatile ConcurrentState concurrent_state = ConcurrentState::IDLE;
        Pool(GcOption option) {
            auto make = [&]() -> std::unique_ptr<std::pmr::memory_resource> {
                if (option._full_debug) {
                    return std::make_unique<std::pmr::monotonic_buffer_resource>();
                } else {
                    return std::make_unique<mi_memory_sourece>();
                    // inner = std::make_unique<RawHeap>();
                }
            };
            if (option.n_collector_threads.has_value()) {
                for (size_t i = 0; i < option.n_collector_threads.value(); i++) {
                    concurrent_resources.emplace_back(std::move(std::make_unique<resouce_t>(make(), true)));
                }
            } else {
                concurrent_resources.emplace_back(std::move(std::make_unique<resouce_t>(make(), option.mode == GcMode::CONCURRENT)));
            }
        }
    };
    GcStats stats_;
    struct ObjectList {
        GcObjectContainer *head = nullptr;
        std::atomic<size_t> count = 0;
        ObjectList() = default;
        ObjectList(const ObjectList &list) : head(list.head), count(list.count.load()) {}
        ObjectList(ObjectList &&list) : head(list.head), count(list.count.load()) {
        }
        void insert(GcObjectContainer *ptr) {
            ptr->next_ = head;
            head = ptr;
            count.fetch_add(1, std::memory_order_relaxed);
        }
    };
    struct ObjectLists {
        using list_t = detail::LockProtected<detail::spin_lock, ObjectList>;
        std::vector<std::unique_ptr<detail::LockProtected<detail::spin_lock, ObjectList>>> lists;
        // void push_to_least_full(GcObjectContainer *ptr) {
        //     while (true) {
        //         auto min = std::numeric_limits<size_t>::max();
        //         size_t idx = 0;
        //         for (size_t i = 0; i < lists.size(); i++) {
        //             auto count = lists[i]->get().count.load();
        //             if (count < min) {
        //                 min = count;
        //                 idx = i;
        //             }
        //         }
        //         if (lists[idx]->lock_enabled()) {
        //             if (lists[idx]->get_lock().try_lock()) {
        //                 lists[idx]->get().insert(ptr);
        //                 lists[idx]->get_lock().unlock();
        //                 return;
        //             }
        //         } else {
        //             lists[idx]->get().insert(ptr);
        //             return;
        //         }
        //     }
        // }
        size_t least_full_list() {
            auto min = std::numeric_limits<size_t>::max();
            size_t idx = 0;
            for (size_t i = 0; i < lists.size(); i++) {
                auto count = lists[i]->get().count.load();
                if (count < min) {
                    min = count;
                    idx = i;
                }
            }
            return idx;
        }
    };
    GcMode mode_ = GcMode::INCREMENTAL;
    size_t max_heap_size_ = 0;
    double gc_threshold_ = 0.5;

    // lock order: object_list -> pool

    detail::LockProtected<detail::shared_lock<detail::spin_lock>, Pool> pool_;

    detail::LockProtected<detail::spin_lock, ObjectLists> object_lists_;
    detail::LockProtected<detail::spin_lock, RootSet> root_set_;
    detail::LockProtected<detail::spin_lock, WorkList> work_list;
    std::optional<std::thread> collector_thread_;
    State state_ = State::IDLE;
    State &state() {
        GC_ASSERT(mode_ != GcMode::CONCURRENT, "State should not be accessed in concurrent mode");
        return state_;
    }
    // free an object. *Be careful*, this function does not update the head pointer or the next pointer of the object
    void free_object(GcObjectContainer *ptr, size_t pool_idx) {
        GC_ASSERT(ptr->pool_idx_ == pool_idx, "Invalid pool index");
        // if (tl_pool_idx.has_value()) {
        //     // GC_ASSERT(tl_pool_idx.value() == pool_idx, "Invalid pool index");
        //     printf("tl_pool_idx = %lld, pool_idx = %lld\n", tl_pool_idx.value(), pool_idx);
        // }
        // stats_.n_collected.fetch_add(1, std::memory_order_relaxed);
        auto &pool = pool_.get();
        // stats_.time_waiting_for_pool += pool_.with_timed([&](auto &pool, auto *lock) {
        if constexpr (is_debug) {
            std::printf("freeing object %p, size=%lld, %lld/%lldB used\n", static_cast<void *>(ptr), ptr->object_size(), pool.allocation_size_.load(), max_heap_size_);
        }
        // pool.allocation_size_.fetch_sub(ptr->object_size(), std::memory_order_relaxed);
        ptr->set_alive(false);
        auto size = ptr->object_size();
        auto align = ptr->object_alignment();
        ptr->~GcObjectContainer();
        pool.concurrent_resources.at(pool_idx)->with([&](auto &resource, auto *lock) {
            resource->deallocate(ptr, size, align);
        });
        // });
    }
    /// @brief `shade` it self do not acquire the lock on work_list
    /// @param ptr
    void shade(const GcObjectContainer *ptr, size_t pool_idx);
    /// @brief scan a gray object and possibly add its children to the work list
    /// scan doees not acquire the lock on work_list
    /// @param ptr
    void scan(const GcObjectContainer *ptr, size_t pool_idx) {
        if (!ptr) {
            return;
        }
        if constexpr (is_debug) {
            std::printf("scanning %p from pool %lld\n", static_cast<const void *>(ptr), pool_idx);
            std::fflush(stdout);
        }
        if (mode() != GcMode::CONCURRENT) {
            GC_ASSERT(ptr->color() == color::GRAY || ptr->is_root(), "Object should be gray");
        }
        if (ptr->color() == color::BLACK) {
            return;
        }
        auto ctx = TracingContext{*this, pool_idx};
        if (auto traceable = ptr->as_tracable()) {
            traceable->trace(Tracer{ctx});
        }
        ptr->set_color(color::BLACK);
    }
    const GcObjectContainer *pop_from_working_list() {
        return work_list.get().pop();
    }
    struct gc_ctor_token_t {};
    void do_incremental(size_t inc_size) {
        GC_ASSERT(state() != State::SWEEPING, "State should not be sweeping");
        if (state() == State::MARKING) {
            // if constexpr (is_debug) {
            //     std::printf("%lld items in work list\n", work_list.get().list.size());
            // }
            auto [mark_end, t_mark] = time_function([this] { return !mark_some(10); });
            stats_.incremental_time += t_mark;
            if (mark_end) {
                auto t = time_function([this] {
                    sweep();
                });
                stats_.incremental_time += t;
                stats_.n_collection_cycles++;
                stats_.collection_time.update(stats_.incremental_time);
                stats_.incremental_time = 0;
                return;
            }
            if (pool_.get().allocation_size_ + inc_size > max_heap_size_) {
                auto t = time_function([this] {
                    while (mark_some(10)) {}
                    sweep();
                });
                stats_.incremental_time += t;
                stats_.n_collection_cycles++;
                stats_.collection_time.update(stats_.incremental_time);
                stats_.incremental_time = 0;
            }
            return;
        }
        GC_ASSERT(work_list.get().empty(), "Work list should be empty");
        bool threshold_condition = pool_.get().allocation_size_ + inc_size > max_heap_size_ * gc_threshold_;
        if constexpr (is_debug) {
            std::printf("allocation_size_ = %llu, max_heap_size_ = %llu, inc_size = %llu\n", pool_.get().allocation_size_.load(), max_heap_size_, inc_size);
            std::printf("threshold_condition = %d\n", threshold_condition);
        }
        if (pool_.get().allocation_size_ + inc_size > max_heap_size_) {
            state() = State::MARKING;
            collect();
            return;
        }
        if (threshold_condition) {
            if constexpr (is_debug) {
                std::printf("threshold condition\n");
            }
            stats_.incremental_time += time_function([this] { scan_roots(); });
        }
    }
    struct gc_memory_resource : std::pmr::memory_resource {
        GcHeap *heap;
        size_t pool_idx;
        gc_memory_resource(GcHeap *heap, size_t pool_idx) : heap(heap), pool_idx(pool_idx) {}
        struct Metadata {
            size_t pool_idx;
        };
        void *do_allocate(std::size_t bytes, std::size_t alignment) override {
            heap->prepare_allocation(bytes + sizeof(Metadata));
            // if (pool_idx != 0) {
            //     std::printf("allocating from pool %lld\n", pool_idx);
            // }
            auto ptr = heap->pool_.with_shared([&](auto &pool, auto *lock) -> void * {
                pool.allocation_size_ += bytes + sizeof(Metadata);
                // auto ptr = reinterpret_cast<uint8_t *>(pool.concurrent_resources.at(pool_idx)->allocate(bytes + sizeof(Metadata), alignment));
                auto ptr = pool.concurrent_resources.at(pool_idx)->with([&](auto &resource, auto *lock) {
                    return reinterpret_cast<uint8_t *>(resource->allocate(bytes + sizeof(Metadata), alignment));
                });
                if constexpr (is_debug) {
                    std::printf("Allocating %p, %lld bytes via pmr, %lld/%lldB used\n", ptr, bytes, pool.allocation_size_.load(), heap->max_heap_size_);
                }
                Metadata meta{pool_idx};
                // if (meta.pool_idx != 0)
                //     std::printf("alloc from pool %lld\n", meta.pool_idx);
                std::memcpy(ptr + bytes, &meta, sizeof(Metadata));
                return ptr;
            },
                                                   heap->mode() == GcMode::CONCURRENT);
            // heap->stats_.time_waiting_for_pool += t;
            return ptr;
        }
        void do_deallocate(void *p,
                           std::size_t bytes,
                           std::size_t alignment) override {
            // if (tl_pool_idx.has_value()) {
            //     // GC_ASSERT(tl_pool_idx.value() == pool_idx, "Invalid pool index");
            //     // printf("tl_pool_idx = %lld, pool_idx = %lld\n", tl_pool_idx.value(), pool_idx);
            // }
            // heap->stats_.time_waiting_for_pool += heap->pool_.with_timed([&](auto &pool, auto *lock) {
            auto &pool = heap->pool_.get();
            pool.allocation_size_.fetch_sub(bytes + sizeof(Metadata), std::memory_order_relaxed);
            if constexpr (is_debug) {
                std::printf("Deallocating %p, %lld bytes via pmr, %lld/%lldB used\n", p, bytes, pool.allocation_size_.load(), heap->max_heap_size_);
            }
            Metadata meta{};
            std::memcpy(&meta, static_cast<uint8_t *>(p) + bytes, sizeof(Metadata));
            // pool.concurrent_resources[meta.pool_idx]->deallocate(p, bytes + sizeof(Metadata), alignment);
            pool.concurrent_resources[meta.pool_idx]->with([&](auto &resource, auto *lock) {
                resource->deallocate(p, bytes + sizeof(Metadata), alignment);
            });
            // });
        }
        bool do_is_equal(const std::pmr::memory_resource &other) const noexcept override {
            return heap == static_cast<const gc_memory_resource &>(other).heap && pool_idx == static_cast<const gc_memory_resource &>(other).pool_idx;
        }
    };
    std::vector<gc_memory_resource> gc_memory_resource_;/// making gc aware of non-traceable memory usage
    // std::condition_variable work_list_non_empty_;
    // std::condition_variable mem_available_;
    std::atomic_bool stop_collector_ = false;
    /*
    Mutator threads only cares about `pool.allocaion_available_` 
     */
    void signal_collection();
    void do_concurrent(size_t inc_size);
    void prepare_allocation(size_t inc_size) {

        if (mode_ == GcMode::INCREMENTAL) {
            do_incremental(inc_size);
        } else if (mode_ == GcMode::CONCURRENT) {
            do_concurrent(inc_size);
        } else {
            if (pool_.get().allocation_size_ + inc_size > max_heap_size_) {
                collect();
            }
        }
    }
    void concurrent_collector();
public:
    GcStats &stats() {
        return stats_;
    }
    std::pmr::memory_resource *memory_resource(size_t pool_idx) {
        // if (pool_idx >= gc_memory_resource_.size()){
        //     printf("pool_idx = %lld, size = %lld\n", pool_idx, gc_memory_resource_.size());
        // }
        // GC_ASSERT(gc_memory_resource_[pool_idx].pool_idx == pool_idx, "Invalid pool index");
        pool_idx = std::min(pool_idx, gc_memory_resource_.size() - 1);
        return &gc_memory_resource_[pool_idx];
    }
    auto &root_set() {
        return root_set_;
    }
    GcHeap(GcOption option, gc_ctor_token_t);
    GcHeap(const GcHeap &) = delete;
    GcHeap &operator=(const GcHeap &) = delete;
    GcHeap(GcHeap &&) = delete;
    GcHeap &operator=(GcHeap &&) = delete;
    GcMode mode() const {
        return mode_;
    }
    static void init(GcOption option = {});
    static void destroy();
    template<class T, class... Args>
        requires std::constructible_from<T, Args...>
    auto *_new_object(std::optional<size_t> preferred_pool_idx, Args &&...args) {
        if constexpr (is_debug) {
            std::printf("Want to allocate %lld bytes\n", sizeof(T));
            std::fflush(stdout);
        }
        prepare_allocation(sizeof(T));
        size_t pool_idx{};
        // auto [ptr, t] =
        auto ptr = pool_.with_shared([&](Pool &pool, auto *lock) {
            if (mode() == GcMode::CONCURRENT) {
                // GC_ASSERT(pool.concurrent_state != ConcurrentState::SWEEPING, "State should not be sweeping");
                stats_.wait_for_atomic_marking += time_function([&] {
                    // while (pool.concurrent_state == ConcurrentState::ATOMIC_MARKING) {
                    //     if constexpr (is_debug) {
                    //         std::printf("Waiting for sweeping\n");
                    //     }
                    //     lock->unlock();
                    //     detail::pause_thread();
                    //     lock->lock();
                    // }
                    lock->wait_shared([this, &pool] { return pool.concurrent_state == ConcurrentState::ATOMIC_MARKING; });
                });
            }
            if (preferred_pool_idx.has_value()) {
                pool_idx = preferred_pool_idx.value();
            } else {
                pool_idx = object_lists_.get().least_full_list();
            }
            GC_ASSERT(pool.allocation_size_ + sizeof(T) <= max_heap_size_, "Out of memory");
            return pool.concurrent_resources.at(pool_idx)->with([&](auto &resource, auto *lock) {
                auto alloc = std::pmr::polymorphic_allocator<T>(resource.get());
                auto ptr = alloc.template allocate_object<T>(1);
                if constexpr (is_debug) {
                    std::printf("Allocated object %p, %lld/%lldB used\n", static_cast<void *>(ptr), pool.allocation_size_.load(), max_heap_size_);
                    std::fflush(stdout);
                }
                pool.allocation_size_ += sizeof(T);
                return ptr;
            });
        },
                                     mode() == GcMode::CONCURRENT);
        stats_.n_allocated.fetch_add(1, std::memory_order_relaxed);
        // stats_.time_waiting_for_pool += t;
        auto offset_of_pool_idx = &reinterpret_cast<T *>(ptr)->pool_idx_ - reinterpret_cast<uint8_t *>(ptr);
        std::memcpy(reinterpret_cast<uint8_t *>(ptr) + offset_of_pool_idx, &pool_idx, sizeof(pool_idx));
        new (ptr) T(std::forward<Args>(args)...);// avoid pmr intercepting the allocator
        GC_ASSERT(sizeof(T) == ptr->object_size(), "size should be the same");
        ptr->set_alive(true);
        ptr->pool_idx_ = static_cast<uint8_t>(pool_idx);
        // stats_.time_waiting_for_pool += 
        pool_.with_shared([&](Pool &pool, auto *lock) {
            if (mode() == GcMode::CONCURRENT) {
                stats_.wait_for_atomic_marking += time_function([&]() {
                    // while (pool.concurrent_state == ConcurrentState::ATOMIC_MARKING) {
                    //     lock->unlock();
                    //     detail::pause_thread();
                    //     lock->lock();
                    // }
                    lock->wait_shared([this, &pool] { return pool.concurrent_state == ConcurrentState::ATOMIC_MARKING; });
                });
            }
            if (mode() != GcMode::STOP_THE_WORLD) {
                // this is necessary, you can't just color it black since the above line `T(std::forward<Args>(args)...);`
                // invokes the constructor and might setup some member pointers
                stats_.time_waiting_for_work_list += work_list.with_timed([&](auto &work_list, auto *lock) {
                    shade(ptr, pool_idx);
                });
            }
            // possible sync issue here
            // while allocation only happens when collector is not in sweeping
            // at this line, the collector might just start sweeping
            stats_.time_waiting_for_object_list += object_lists_.with_timed([&](auto &lists, auto *lock) {
                // if (list.head == nullptr) {
                //     list.head = ptr;
                // } else {
                //     ptr->next_ = list.head;
                //     list.head = ptr;
                // }
                lists.lists.at(pool_idx)->with([&](auto &list, auto *lock) {
                    list.insert(ptr);
                });
            });
        });
        return ptr;
    }
    void collect();
    void sweep();
    std::tuple<size_t, size_t, GcObjectContainer *, GcObjectContainer *> sweep_list(ObjectList &list, size_t pool_idx);
    void scan_roots();
    bool mark_some(size_t max_count);
    void parallel_marking();
    // void add_to_working_list(const GcObjectContainer *ptr) {
    //     if constexpr (is_debug) {
    //         std::printf("adding %p to work list\n", static_cast<const void *>(ptr));
    //     }
    //     work_list.get().append(ptr);
    // }
    void add_to_working_list(const GcObjectContainer *ptr, size_t pool_idx) {
        if constexpr (is_debug) {
            std::printf("adding %p to work list %lld\n", static_cast<const void *>(ptr), pool_idx);
        }
        work_list.get().lists.at(pool_idx)->with([&](auto &list, auto *lock) {
            list.push_back(ptr);
        });
    }
    ~GcHeap() {
        if (stop_collector_) {
            return;
        }
        stop();
    }
    bool need_write_barrier() {
        if (mode_ == GcMode::STOP_THE_WORLD) {
            return false;
        } else if (mode_ == GcMode::CONCURRENT) {
            return true;
        }
        // TODO: what about concurrent mode?
        return state() == State::MARKING;
    }
    bool is_paralle_collection() const {
        return worker_pool_.has_value();
    }
private:
    void stop() {
        stop_collector_ = true;
        if (collector_thread_.has_value()) {
            collector_thread_->join();
        }
        collect();
        // GC_ASSERT(object_lists_.get().head == nullptr, "Memory leak detected");
        for (auto &list : object_lists_.get().lists) {
            GC_ASSERT(list->get().head == nullptr, "Memory leak detected");
        }
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
    template<class U>
    friend class GarbageCollected;
    // here is the traceable implementation
    T *container_;
    template<class U>
    friend struct apply_trace;


public:
    void reset() {
        container_ = nullptr;
    }
    GcPtr() : container_(nullptr) {}
    explicit GcPtr(T *container_) : container_(container_) {}
    T *operator->() const {
        detail::check_alive(container_);
        return static_cast<T *>(container_);
    }
    T &operator*() const {
        detail::check_alive(container_);
        return *static_cast<T *>(container_);
    }
    T *get() const {
        return static_cast<T *>(container_);
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
    std::optional<RootSet::Node> &root_node() const {
        static_assert(std::is_base_of_v<GcObjectContainer, T>, "T should be a subclass of GcObjectContainer");
        return container_->root_node;
    }
    operator bool() const {
        return container_ != nullptr;
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
template<class U>
class Member;
/// @brief an on stack handle to a gc object
template<class T>
class Local {
    template<class U>
    friend class Member;
    GcPtr<T> ptr_;
    void inc() {
        if (ptr_.gc_object_container()) {
            ptr_.gc_object_container()->inc_root_ref_count();
            if (ptr_.gc_object_container()->root_ref_count == 1) {
                // become a new root, add to the root set
                auto &heap = get_heap();
                heap.stats_.time_waiting_for_root_set += heap.root_set().with_timed([&](auto &rs, auto *lock) {
                    auto node = rs.add(ptr_.gc_object_container());
                    if constexpr (is_debug) {
                        std::printf("adding root %p\n", static_cast<const void *>(ptr_.gc_object_container()));
                    }
                    ptr_.root_node().emplace(node);
                });
                if (ptr_.gc_object_container()->color() == color::WHITE) {
                    heap.stats_.time_waiting_for_work_list += heap.work_list.with_timed([&](auto &work_list, auto *lock) {
                        heap.shade(ptr_.gc_object_container(), work_list.least_filled());
                    });
                }
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

                auto &heap = get_heap();
                heap.stats_.time_waiting_for_root_set += heap.root_set().with_timed([&](auto &rs, auto *lock) {
                    rs.remove(*ptr_.root_node());
                    ptr_.root_node().reset();
                });
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
    Local(GcPtr<T> ptr) : ptr_(ptr) {
        inc();
    }
    Local(const Member<T> &member) : ptr_(member.ptr_) {
        inc();
    }

    template<class... Args>
        requires std::constructible_from<T, Args...>
    static Local make(Args &&...args) {
        auto &heap = get_heap();
        auto gc_ptr = GcPtr<T>{heap._new_object<T>(std::nullopt, std::forward<Args>(args)...)};
        return Local(gc_ptr);
    }
    template<class... Args>
        requires std::constructible_from<T, Args...>
    static Local make_in_pool(size_t pool_idx, Args &&...args) {
        auto &heap = get_heap();
        auto gc_ptr = GcPtr<T>{heap._new_object<T>(pool_idx, std::forward<Args>(args)...)};
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
    Local &operator=(const Member<T> &member) {
        dec();
        ptr_ = member.ptr_;
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
    GcPtr<T> get() const {
        return ptr_;
    }
};
template<class T>
class Member {
    template<class U>
    friend struct apply_trace;
    template<class U>
    friend class GcArray;
    template<class U>
    friend class Local;
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
            if (parent_->color() == color::BLACK) {
                if constexpr (is_debug) {
                    std::printf("write barrier, color=%d\n", ptr.gc_object_container()->color());
                }
                heap.stats_.wait_for_atomic_marking += heap.work_list.with_timed([&](auto &work_list, auto *lock) {
                    heap.shade(ptr.gc_object_container(), work_list.least_filled());
                });
            }
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
    operator GcPtr<T>() const {
        return ptr_;
    }
    operator bool() const {
        return ptr_ != nullptr;
    }
    GcPtr<T> get() const {
        return ptr_;
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
        auto alloc = std::pmr::polymorphic_allocator(heap.memory_resource(pool_idx()));
        data_ = alloc.template allocate_object<Member<T>>(n);
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
        auto alloc = std::pmr::polymorphic_allocator(heap.memory_resource(pool_idx()));
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
        return Local<GcArray<T>>::make_in_pool(pool_idx(), len);
    }
    void ensure_size(size_t new_size) {
        if (data_ == nullptr) {
            data_ = alloc(std::max<size_t>(16, new_size));
            return;
        }
        if (new_size <= data_->size()) {
            return;
        }
        auto new_capacity = std::max<size_t>(16ull, std::max(data_->size() * 2, new_size));
        Local<GcArray<T>> new_data = alloc(new_capacity);
        for (size_t i = 0; i < data_->size(); i++) {
            (*new_data)[i] = (*data_)[i];
        }
        data_ = new_data;
    }
public:
    struct Iter : public GarbageCollected<Iter> {
        Member<GcVector<T>> vec_;
        size_t idx_;
        Iter(GcPtr<GcVector<T>> vec, size_t idx) : vec_(this), idx_(idx) {
            vec_ = vec;
        }
        GC_CLASS(vec_)
        Member<T> &operator*() {
            return (*vec_)[idx_];
        }
        Iter &operator++() {
            idx_++;
            return *this;
        }

        Iter operator++(int) {
            Iter temp(vec_, idx_);
            idx_++;
            return temp;
        }
        bool operator!=(const Iter &other) const {
            return idx_ != other.idx_;
        }
        bool operator==(const Iter &other) const {
            return idx_ == other.idx_;
        }
    };
    Iter begin() {
        return Iter(gc_ptr_from_this(), 0);
    }
    Iter end() {
        return Iter(gc_ptr_from_this(), size_);
    }
    size_t capacity() const {
        return data_->size();
    }
    GcVector() : data_(this), size_(0) {}
    void push_back(GcPtr<T> value) {
        ensure_size(size_ + 1);
        (*data_)[size_++] = value;
    }
    Member<T> &operator[](size_t idx) {
        return (*data_)[idx];
    }
    Member<T> &at(size_t idx) const {
        if (idx >= size_) {
            throw std::out_of_range("Index out of range");
        }
        return (*data_)[idx];
    }
    void pop_back() {
        at(size_ - 1) = nullptr;
        size_--;
    }
    Member<T> &back() {
        GC_ASSERT(size_ > 0, "Size should be greater than 0");
        return (*data_)[size_ - 1];
    }
    size_t size() const {
        return size_;
    }
    GC_CLASS(data_)
};
template<class K, class V>
class GcHashMap : public GarbageCollected<GcHashMap<K, V>> {
    struct Bucket : public Traceable {
        Member<K> key;
        Member<V> value;
        Member<Bucket> next;
        GC_CLASS(key, value, next)
        Bucket() : key(this), value(this), next(this) {}
    };
    Member<GcArray<Bucket>> data_;
    size_t total_size_ = 0;
    double load_factor() const {
        return static_cast<double>(total_size_) / data_->size();
    }
    void rehash() {
        auto new_hashmap = Local<GcHashMap<K, V>>::make(data_->size() * 2);
        for (auto [k, v] : *this) {
            new_hashmap->insert(k, v);
        }
        data_ = new_hashmap->data_;
    }
public:
    size_t size() const {
        return total_size_;
    }
    struct Iter : public GarbageCollected<Iter> {
        Member<GcHashMap<K, V>> map_;
        size_t idx_;
        Member<Bucket> current_;
        GC_CLASS(map_, current_)
        Iter(GcPtr<GcHashMap<K, V>> map, size_t idx, GcPtr<Bucket> current) : map_(this), idx_(idx), current_(this) {
            map_ = map;
            current_ = current;
        }
        std::pair<GcPtr<K>, GcPtr<V>> operator*() {
            GC_ASSERT(current_, "Invalid iterator");
            return {current_->key, current_->value};
        }
        void advance() {
            if (current_ == nullptr) {
                return;
            }
            if (current_->next) {
                current_ = current_->next;
                return;
            }
            for (size_t i = idx_ + 1; i < map_->data_->size(); i++) {
                if ((*map_->data_)[i]) {
                    idx_ = i;
                    current_ = (*map_->data_)[i];
                    return;
                }
            }
            current_ = nullptr;
        }
        bool operator!=(const Iter &other) const {
            return current_ != other.current_;
        }
        Iter &operator++() {
            advance();
            return *this;
        }
        Iter operator++(int) {
            Iter temp = *this;
            advance();
            return temp;
        }
    };
    Iter begin() {
        for (size_t i = 0; i < data_->size(); i++) {
            if ((*data_)[i]) {
                return Iter(gc_ptr_from_this(), i, (*data_)[i]);
            }
        }
        return end();
    }
    Iter end() {
        return Iter(gc_ptr_from_this(), std::numeric_limits<size_t>::max(), GcPtr<Bucket>(nullptr));
    }
    GcHashMap(size_t hash_size = 64) : data_(this) {
        data_ = Local<GcArray<Bucket>>::make(hash_size);
    }
    void insert(std::pair<gc::GcPtr<K>, gc::GcPtr<V>> pair) {
        insert(pair.first, pair.second);
    }
    void insert(gc::GcPtr<K> key, gc::GcPtr<V> value) {
        auto idx = bucket_index(key);
        Member<Bucket> &bucket = (*data_)[idx];
        GcPtr<Bucket> current = bucket;
        while (current) {
            if (current->key == key) {
                current->value = value;
                return;
            }
            current = current->next;
        }
        auto new_bucket = Local<Bucket>::make();
        new_bucket->key = key;
        new_bucket->value = value;
        new_bucket->next = bucket;
        bucket = new_bucket;
        total_size_++;
        if (load_factor() > 0.75) {
            rehash();
        }
    }
    bool contains(gc::GcPtr<K> key) {
        auto idx = bucket_index(key);
        Member<Bucket> &bucket = (*data_)[idx];
        GcPtr<Bucket> current = bucket;
        while (current) {
            if (current->key == key) {
                return true;
            }
            current = current->next;
        }
        return false;
    }
    gc::GcPtr<V> at(gc::GcPtr<K> key) {
        auto idx = bucket_index(key);
        auto &bucket = (*data_)[idx];
        auto current = bucket;
        while (current) {
            if (current->key == key) {
                return current->value;
            }
            current = current->next;
        }
        GC_ASSERT(false, "Key not found");
    }
    GC_CLASS(data_)
private:
    size_t bucket_index(gc::GcPtr<K> key) {
        return std::hash<K>{}(*key.get()) % data_->size();
    }
};
}// namespace gc
template<class T>
struct std::hash<gc::GcPtr<T>> {
    size_t operator()(const gc::GcPtr<T> &ptr) const {
        return std::hash<const gc::GcObjectContainer *>()(ptr.gc_object_container());
    }
};
template<class T>
struct std::hash<gc::Adaptor<T>> {
    size_t operator()(const gc::Adaptor<T> &obj) const {
        return std::hash<T>{}(obj);
    }
};