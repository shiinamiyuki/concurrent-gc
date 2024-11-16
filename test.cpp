#include "gc.h"
#include "rc.h"
#include <print>
struct Bar : gc::Traceable {
    int val;
    explicit Bar(int val = 0) : val(val) {}
    void trace(const gc::Tracer &) const override {}
};
struct Foo : gc::Traceable {
    gc::GcPtr<Bar> bar;
    gc::GcPtr<std::string> s;
    Foo() = default;
    void trace(const gc::Tracer &tr) const override {
        tr(bar, s);
    }
};
int main() {
    {
        auto bar = gc::make_gc_ptr<Bar>(1234);
        auto foo = gc::Local{gc::make_gc_ptr<Foo>()};
        foo->bar = bar;
        gc::get_heap().collect();
        std::print("{}\n", bar->val);
    }
    return 0;
}