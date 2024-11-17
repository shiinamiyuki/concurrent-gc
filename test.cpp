#include "gc.h"
#include "rc.h"
#include <print>
struct Bar : gc::Traceable {
    int val;
    explicit Bar(int val = 0) : val(val) {}
    void trace(const gc::Tracer &) const override {}
};
struct Foo : gc::Traceable {
    gc::Member<Bar> bar;
    gc::Member<std::string> s;
    Foo() : bar(this), s(this) {}
    void trace(const gc::Tracer &tr) const override {
        tr(bar, s);
    }
};
int main() {
    {
        // auto bar = gc::make_gc_ptr<Bar>(1234);
        // auto foo = gc::Local{gc::make_gc_ptr<Foo>()};
        // foo->bar = bar;
        // gc::get_heap().collect();
        // std::print("{}\n", bar->val);
        auto bar = gc::Local<Bar>::make(1234);
        auto foo = gc::Local<Foo>::make();
        foo->bar = bar;
        gc::get_heap().collect();
        std::print("{}\n", bar->val);
    }
    return 0;
}