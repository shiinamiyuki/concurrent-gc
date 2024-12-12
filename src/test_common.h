#pragma once
#include "gc.h"
#include "rc.h"
#include <sstream>
struct GcPolicy {
    template<class T>
    using Ptr = gc::GcPtr<T>;
    template<class T>
    using Owned = gc::Local<T>;
    template<class T>
    using Member = gc::Member<T>;
    template<class T>
    using Array = gc::GcVector<T>;
    template<class T>
    using Enable = gc::GarbageCollected<T>;

    template<class K, class V>
    using HashMap = gc::GcHashMap<K, V>;

    gc::GcOption option{};
    void init() {
        gc::GcHeap::init(option);
    }
    void finalize() {
        gc::GcHeap::destroy();
    }

    template<class T, class... Args>
    static Owned<T> make(Args &&...args) {
        return gc::Local<T>::make(std::forward<Args>(args)...);
    }
    std::string name() const {
        std::stringstream ss;
        ss << "GC " << gc::to_string(option.mode);
        if (option.n_collector_threads.has_value()) {
            ss << "@" << option.n_collector_threads.value() << "T";
        }
        return ss.str();
    }
};
template<class T, class CounterPolicy>
struct RcHasher {
    size_t operator()(const rc::RcPtr<T, CounterPolicy> &ptr) const {
        return std::hash<T>()(*ptr);
    }
};
template<class T, class CounterPolicy>
struct RcEqual {
    bool operator()(const rc::RcPtr<T, CounterPolicy> &lhs, const rc::RcPtr<T, CounterPolicy> &rhs) const {
        return *lhs == *rhs;
    }
};
template<class T, class CounterPolicy>
struct RcVec : public rc::RcFromThis<RcVec<T, CounterPolicy>, CounterPolicy>, std::pmr::vector<rc::RcPtr<T, CounterPolicy>> {
    using Base = std::pmr::vector<rc::RcPtr<T, CounterPolicy>>;
    using Base::Base;
};
template<class K, class V, class CounterPolicy>
struct RcHashMap : public rc::RcFromThis<RcHashMap<K, V, CounterPolicy>, CounterPolicy>, std::pmr::unordered_map<rc::RcPtr<K, CounterPolicy>, rc::RcPtr<V, CounterPolicy>, RcHasher<K, CounterPolicy>, RcEqual<K, CounterPolicy>> {
    using Base = std::pmr::unordered_map<rc::RcPtr<K, CounterPolicy>, rc::RcPtr<V, CounterPolicy>, RcHasher<K, CounterPolicy>, RcEqual<K, CounterPolicy>>;
    using Base::Base;
};
template<class CounterPolicy>
struct RcPolicy {
    template<class T>
    using Ptr = T *;
    template<class T>
    using Owned = rc::RcPtr<T, CounterPolicy>;
    template<class T>
    using Member = rc::DummyMember<T, CounterPolicy>;
    template<class T>
    using Array = RcVec<T, CounterPolicy>;
    template<class T>
    using Enable = rc::RcFromThis<T, CounterPolicy>;

    template<class K, class V>
    using HashMap = RcHashMap<K, V, CounterPolicy>;

    std::pmr::memory_resource *old_resource{};
    void init() {
        old_resource = std::pmr::set_default_resource(CounterPolicy::resource.get());
    }
    void finalize() {
        std::pmr::set_default_resource(old_resource);
    }

    template<class T, class... Args>
    static Owned<T> make(Args &&...args) {
        return Owned<T>(typename Owned<T>::init_t{}, std::forward<Args>(args)...);
    }
    std::string name() const {
        if constexpr (std::is_same_v<CounterPolicy, rc::RefCounter>) {
            return "RC RefCounter";
        } else {
            return "RC AtomicRefCounter";
        }
    }
};


// https://en.wikipedia.org/wiki/Permuted_congruential_generator
struct Rng {
    uint64_t state = 0x4d595df4d0f33173;// Or something seed-dependent
    uint64_t const multiplier = 6364136223846793005u;
    uint64_t const increment = 1442695040888963407u;// Or an arbitrary odd constant

    uint32_t rotr32(uint32_t x, unsigned r) {
        return x >> r | x << (-r & 31);
    }

    uint32_t pcg32() {
        uint64_t x = state;
        unsigned count = (unsigned)(x >> 59);// 59 = 64 - 5

        state = x * multiplier + increment;
        x ^= x >> 18;                             // 18 = (64 - 27)/2
        return rotr32((uint32_t)(x >> 27), count);// 27 = 32 - 5
    }

    Rng(uint64_t seed) {
        state = seed + increment;
        (void)pcg32();
    }
};