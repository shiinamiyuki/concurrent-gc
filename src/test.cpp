
#include "gc.h"
#include "rc.h"
#include <print>
#include <array>
struct Bar : gc::GarbageCollected<Bar> {
    int val;
    explicit Bar(int val = 0) : val(val) {}
    void trace(const gc::Tracer &) const override {}
};

struct Foo : gc::GarbageCollected<Foo> {
    gc::Member<Bar> bar;
    gc::Member<gc::Adaptor<std::string>> s;
    gc::Member<Foo> foo;
    Foo() : bar(this), s(this), foo(this) {}
    void trace(const gc::Tracer &tr) const override {
        tr(bar, s, foo);
    }
};
void test_wb() {
    gc::GcHeap::init();
    // std::println("hello");
    auto &heap = gc::get_heap();
    gc::Local<Foo> foo1 = gc::Local<Foo>::make();

    foo1->bar = gc::Local<Bar>::make(1234);
    foo1->s = gc::Local<gc::Adaptor<std::string>>::make("hello");
    // heap.mark_some();
    //    std::fflush(stdout);
    // printf("foo1->bar = %d\n", foo1->bar->val);
    gc::Local<Foo> foo2 = gc::Local<Foo>::make();
    foo1->foo = foo2;
    foo2->bar = gc::Local<Bar>::make(5678);
    while (heap.mark_some()) {}
    heap.mark_some();
    heap.mark_some();
    heap.mark_some();
    heap.mark_some();
    GC_ASSERT(foo1.gc_object_container() == foo1->bar.parent(), "foo1 and bar should be in the same container");
    printf("is black = %d\n", foo1->bar.parent()->color() == gc::color::BLACK);
    printf("is white = %d\n", foo2->bar.gc_object_container()->color() == gc::color::WHITE);
    foo1->bar = foo2->bar;
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
void test_random() {
    gc::GcOption option{};
    using Big = std::array<float, 1024>;
    option.max_heap_size = 1024 * 16;
    gc::GcHeap::init(option);
    // for (auto i = 0; i < 1000; i++) {
        for (auto j = 0; j < 10000; j++) {
            auto v = gc::Local<gc::Adaptor<Big>>::make();
            // GC_ASSERT(v.gc_object_container()->object_size() == sizeof(Big), "size should be sizeof(Big)");
            // gc::get_heap().collect();
        }
    // }
}
int main() {
    test_random();
    return 0;
}