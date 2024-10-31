#include "gc.h"
#include <print>
struct Bar : gc::Traceable {
    int val;
    explicit Bar(int val = 0) : val(val) {}
    void trace(const gc::TracingCallback &) const override {}
};
struct Foo : gc::Traceable {
    gc::GcPtr<Bar> bar;
    void trace(const gc::TracingCallback &) const override {}
};
int main() {
    auto bar = gc::Local{gc::make_gc_ptr<Bar>(1)};
    gc::get_heap().collect();
    std::print("{}\n", bar->val);
    return 0;
}