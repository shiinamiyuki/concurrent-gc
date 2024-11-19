
#include "gc.h"
#include "rc.h"
#include <print>
#include <random>
#include <array>
struct Bar : gc::Traceable {
    int val;
    explicit Bar(int val = 0) : val(val) {}
    GC_CLASS()
};

struct Foo : gc::Traceable {
    gc::Member<Bar> bar;
    gc::Member<gc::Adaptor<std::string>> s;
    gc::Member<Foo> foo;
    Foo() : bar(this), s(this), foo(this) {}
    GC_CLASS(bar, s, foo)
};
// void test_wb() {
//     gc::GcHeap::init();
//     // std::println("hello");
//     auto &heap = gc::get_heap();
//     gc::Local<Foo> foo1 = gc::Local<Foo>::make();

//     foo1->bar = gc::Local<Bar>::make(1234);
//     foo1->s = gc::Local<gc::Adaptor<std::string>>::make("hello");
//     // heap.mark_some();
//     //    std::fflush(stdout);
//     // printf("foo1->bar = %d\n", foo1->bar->val);
//     gc::Local<Foo> foo2 = gc::Local<Foo>::make();
//     foo1->foo = foo2;
//     foo2->bar = gc::Local<Bar>::make(5678);
//     while (heap.mark_some()) {}
//     heap.mark_some();
//     heap.mark_some();
//     heap.mark_some();
//     heap.mark_some();
//     GC_ASSERT(foo1.gc_object_container() == foo1->bar.parent(), "foo1 and bar should be in the same container");
//     printf("is black = %d\n", foo1->bar.parent()->color() == gc::color::BLACK);
//     printf("is white = %d\n", foo2->bar.gc_object_container()->color() == gc::color::WHITE);
//     foo1->bar = foo2->bar;
//     while (heap.mark_some()) {}
// }
// void test_simple() {
//     auto &heap = gc::get_heap();
//     gc::Local<Foo> foo;
//     {
//         // auto bar = gc::make_gc_ptr<Bar>(1234);
//         // auto foo = gc::Local{gc::make_gc_ptr<Foo>()};
//         // foo->bar = bar;
//         // gc::get_heap().collect();
//         // std::print("{}\n", bar->val);
//         {
//             auto bar = gc::Local<Bar>::make(1234);
//             foo = gc::Local<Foo>::make();
//             foo->s = gc::Local<gc::Adaptor<std::string>>::make("hello");
//             foo->bar = bar;
//         }
//         while (heap.mark_some()) {}
//         // gc::get_heap().collect();
//         std::print("{}\n", foo->bar->val);
//         while (heap.mark_some()) {}
//         foo->bar = nullptr;
//         std::print("foo->s = {}", static_cast<std::string &>(*foo->s));
//     }
//     heap.collect();
//     std::print("foo still alive\n");
// }

void test_random() {
    gc::GcOption option{};
    // option.mode = gc::GcMode::STOP_THE_WORLD;
    // using Big = std::pmr::vector<float>;
    option.max_heap_size = 1024;
    gc::GcHeap::init(option);
    auto &heap = gc::get_heap();
    using FloatVec = gc::GcVector<gc::Boxed<float>>;
    sizeof(FloatVec);

    // using FloatVec = gc::Adaptor<std::pmr::vector<float>>;
    // for (auto i = 0; i < 1000; i++) {
    for (auto j = 0; j < 1000; j++) {
        // auto alloc = std::pmr::polymorphic_allocator(heap.memory_resource());
        auto v = gc::Local<FloatVec>::make();
        GC_ASSERT(sizeof(FloatVec) == v->object_size(), "size should be the same");
        // v->resize(100);
        for (auto i = 0; i < 10; i++) {
            v->push_back(gc::Local<gc::Boxed<float>>::make(static_cast<float>(i)));
        }
    }
    // }
}
template<typename T>
struct Node : public gc::Traceable {
    T val{};
    gc::Member<gc::GcVector<Node<T>>> children;
    Node() : children(this) {
        children = gc::Local<gc::GcVector<Node<T>>>::make();
    }
    GC_CLASS(children)
};
void test_random_graph() {
    gc::GcOption option{};
    option.max_heap_size = 1024 * 64;
    gc::GcHeap::init(option);
    std::random_device rd;
    std::uniform_int_distribution<int> gen;
    for (auto j = 0; j < 20; j++) {
        std::vector<gc::Local<Node<int>>> nodes;
        for (int i = 0; i < 100; i++) {
            auto node = gc::Local<Node<int>>::make();
            node->val = gen(rd);
            nodes.push_back(node);
        }
        for (int i = 0; i < 100; i++) {
            auto& node = nodes[i];
            for (int j = 0; j < 10; j++) {
                node->children->push_back(nodes[gen(rd) % nodes.size()]);
            }
        }
    }
}
// void test_random() {
//     gc::GcOption option{};
//     option.mode = gc::GcMode::STOP_THE_WORLD;
//     using Big = std::pmr::vector<float>;
//     option.max_heap_size = 1024 * 10;
//     gc::GcHeap::init(option);
//     auto &heap = gc::get_heap();
//     // for (auto i = 0; i < 1000; i++) {
//     for (auto j = 0; j < 1000; j++) {
//         auto alloc = std::pmr::polymorphic_allocator<float>(heap.memory_resource());
//         // gc::Adaptor<std::pmr::vector<float>> v(alloc);
//         // GC_ASSERT(v.get_allocator().resource()->is_equal(*heap.memory_resource()), "memory resource should be the same");
//         // auto v2 = std::pmr::vector<float>(std::move(v));
//         // GC_ASSERT(v2.get_allocator().resource()->is_equal(*heap.memory_resource()), "memory resource should be the same");
//         // alloc.new_object<std::pmr::vector<float>>(alloc, alloc);
//         // v.resize(1000);
//         auto vec = gc::Local<gc::Adaptor<std::pmr::vector<float>>>::make(alloc);

//         vec->resize(10);
//         GC_ASSERT(vec->get_allocator().resource()->is_equal(*heap.memory_resource()), "memory resource should be the same");
//         printf("vec size = %lld\n", vec->size());
//         // GC_ASSERT(v.gc_object_container()->object_size() == sizeof(Big), "size should be sizeof(Big)");
//         // gc::get_heap().collect();
//     }
//     // }
// }
int main() {
    test_random_graph();
    return 0;
}