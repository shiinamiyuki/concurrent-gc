#include "gc.h"
#include <print>
struct Bar : gc::Traceable {
    int val;
    explicit Bar(int val = 0) : val(val) {}
    void trace(const gc::Tracer &) const override {}
};
struct Foo : gc::Traceable {
    gc::GcPtr<Bar> bar;
    Foo() = default;
    void trace(const gc::Tracer &tr) const override {
        tr(bar);
    }
};
int main() {
    {
        auto bar = gc::make_gc_ptr<Bar>(1);
        auto foo = gc::Local{gc::make_gc_ptr<Foo>()};
        foo->bar = bar;
        gc::get_heap().collect();
        std::print("{}\n", bar->val);
    }
    gc::get_heap().collect();
    return 0;
}