
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
    gc::Member<gc::Adaptor<std::string>> s;
    gc::Member<Foo> foo;
    Foo() : bar(this), s(this), foo(this) {}
    void trace(const gc::Tracer &tr) const override {
        tr(bar, s, foo);
    }
};
void test_wb() {
    // std::println("hello");
    auto &heap = gc::get_heap();
    gc::Local<Foo> foo1 = gc::Local<Foo>::make();

    foo1->bar = gc::Local<Bar>::make(1234);
    // heap.mark_some();
    //    std::fflush(stdout);
    // printf("foo1->bar = %d\n", foo1->bar->val);
    gc::Local<Foo> foo2 = gc::Local<Foo>::make();
    foo1->foo = foo2;
    while (heap.mark_some()) {}
    // heap.mark_some();
    printf("is white = %d\n", heap.is_white(foo1->bar.gc_object_container()));
    foo2->bar = foo1->bar;
    while (heap.mark_some()) {}
}
void test_simple() {
    auto &heap = gc::get_heap();
    gc::Local<Foo> foo;
    {
        // auto bar = gc::make_gc_ptr<Bar>(1234);
        // auto foo = gc::Local{gc::make_gc_ptr<Foo>()};
        // foo->bar = bar;
        // gc::get_heap().collect();
        // std::print("{}\n", bar->val);
        {
            auto bar = gc::Local<Bar>::make(1234);
            foo = gc::Local<Foo>::make();
            foo->s = gc::Local<gc::Adaptor<std::string>>::make("hello");
            foo->bar = bar;
        }
        while (heap.mark_some()) {}
        // gc::get_heap().collect();
        std::print("{}\n", foo->bar->val);
        while (heap.mark_some()) {}
        foo->bar = nullptr;
        std::print("foo->s = {}", static_cast<std::string &>(*foo->s));
    }
    heap.collect();
    std::print("foo still alive\n");
}

int main() {
    test_wb();
    return 0;
}