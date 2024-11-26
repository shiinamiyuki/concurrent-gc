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
        return ss.str();
    }
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
    using Array = std::pmr::vector<rc::RcPtr<T, CounterPolicy>>;
    template<class T>
    using Enable = rc::RcFromThis<T>;

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

#define IMPORT_TYPES()                          \
    template<class U>                           \
    using Ptr = typename U::template Ptr<U>;       \
    template<class U>                           \
    using Owned = typename U::template Ownedr<U>;   \
    template<class U>                           \
    using Member = typename U::template Memberr<U>; \
    template<class U>                           \
    using Array = typename U::template Arrayr<U>;   \
    template<class U>                           \
    using Enable = typename U::template Enabler<U>;
