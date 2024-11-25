#pragma once
#include "gc.h"
#include "rc.h"


struct GcPolicy {
    template<class T>
    using Ptr = gc::GcPtr<T>;
    template<class T>
    using Owned = gc::Local<T>;
    template<class T>
    using Member = gc::Member<T>;
    template<class T>
    using Array = gc::GcVector<T>;
    using Enable = gc::Traceable;
};
template<class CounterPolicy>
struct RcPolicy {
    template<class T>
    using Ptr = T*;
    template<class T>
    using Owned = rc::RcPtr<T, CounterPolicy>;
    template<class T>
    using Member = rc::DummyMember<T, CounterPolicy>;
};