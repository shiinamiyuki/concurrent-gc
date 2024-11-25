
#include "gc.h"
#include "rc.h"
#include <array>
#include <numeric>
#include <chrono>
#include "test_common.h"
using StatsTracker = gc::StatsTracker;
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
template<typename T>
auto find_all_nodes(gc::GcPtr<Node<T>> root) {
    std::vector<gc::GcPtr<Node<T>>> nodes;
    std::unordered_set<Node<T> *>
        visited;
    std::function<void(gc::GcPtr<Node<T>>)> dfs = [&](gc::GcPtr<Node<T>> node) {
        if (visited.contains(node.get())) {
            return;
        }
        visited.insert(node.get());
        nodes.push_back(node);
        auto num_children = node->children->size();
        for (auto i = 0; i < num_children; i++) {
            dfs(node->children->at(i));
        }
    };
    dfs(root);
    return nodes;
}

struct WBTestNode : public gc::Traceable {
    int val{};
    gc::Member<WBTestNode> left;
    gc::Member<WBTestNode> right;
    WBTestNode() : left(this), right(this) {}
    GC_CLASS(left, right)
};
void test_wb() {
    gc::GcOption option{};
    option.max_heap_size = 1024 * 180;
    option._full_debug = true;
    option.mode = gc::GcMode::INCREMENTAL;
    gc::GcHeap::init(option);
    for (auto j = 0; j < 10; j++) {
        std::vector<gc::Local<WBTestNode>> roots;
        for (int i = 0; i < 100; i++) {
            auto node = gc::Local<WBTestNode>::make();
            node->val = i;
            auto x = gc::Local<WBTestNode>::make();
            auto y = gc::Local<WBTestNode>::make();
            x->val = 2 * i;
            y->val = 2 * i + 1;
            node->left = x;
            node->right = y;
            auto z = gc::Local<WBTestNode>::make();
            z->val = 2 * i + 2;
            y->left = z;
            roots.push_back(node);
        }
        gc::get_heap().collect();
        gc::get_heap().scan_roots();
        GC_ASSERT(gc::get_heap().mark_some(1), "should mark some");
        for (auto &root : roots) {
            auto &x = root->left;
            auto &y = root->right;
            auto &z = y->left;
            std::printf("root->color = %d\n", root->color());
            std::printf("x->color = %d\n", x->color());
            std::printf("y->color = %d\n", y->color());
            std::printf("z->color = %d\n", z->color());
            x->left = z;
            y->left = nullptr;
        }
        while (gc::get_heap().mark_some(1)) {}
        gc::get_heap().sweep();
        for (auto &root : roots) {
            auto &x = root->left;
            auto &z = x->left;
            std::printf("z->val = %d\n", z->val);
        }
    }
}
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

void bench_allocation() {
    printf("Running allocation benchmark\n");
    auto bench = [](gc::GcMode mode) {
        gc::GcOption option{};
        option.mode = mode;
        option.max_heap_size = 1024 * 64;
        gc::GcHeap::init(option);
        StatsTracker tracker;
        auto f = [&] {
            for (auto j = 0; j < 400; j++) {
                gc::Local<Node<int>> root = gc::Local<Node<int>>::make();
                auto time = std::chrono::high_resolution_clock::now();
                for (int i = 0; i < 20; i++) {
                    auto node = gc::Local<Node<int>>::make();
                    node->val = i;
                    root->children->push_back(node);
                }
                auto elapsed = std::chrono::high_resolution_clock::now() - time;
                tracker.update(static_cast<double>(elapsed.count()) * 1e-3);
                // std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        };
        printf("benchmarking %s\n", gc::to_string(mode));
        f();
        tracker = StatsTracker{};
        f();
        printf("mean = %f\n", tracker.mean);
        printf("max = %f\n", tracker.max);
        printf("min = %f\n", tracker.min);
        printf("variance = %f\n", tracker.variance());
        gc::GcHeap::destroy();
    };
    bench(gc::GcMode::STOP_THE_WORLD);
    bench(gc::GcMode::INCREMENTAL);
    bench(gc::GcMode::CONCURRENT);
}
void bench_allocation_collect() {
    printf("Running allocation + collect benchmark\n");
    auto bench = [](gc::GcMode mode) {
        gc::GcOption option{};
        option.mode = mode;
        option.max_heap_size = 1024 * 64;
        gc::GcHeap::init(option);
        StatsTracker tracker;
        auto f = [&] {
            for (auto j = 0; j < 400; j++) {
                gc::Local<Node<int>> root = gc::Local<Node<int>>::make();
                auto time = std::chrono::high_resolution_clock::now();
                for (int i = 0; i < 20; i++) {
                    auto node = gc::Local<Node<int>>::make();
                    node->val = i;
                    root->children->push_back(node);
                }
                // simulate some work
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                auto elapsed = std::chrono::high_resolution_clock::now() - time;
                tracker.update(static_cast<double>(elapsed.count()) * 1e-3);
            }
        };
        printf("benchmarking %s\n", gc::to_string(mode));
        f();
        tracker = StatsTracker{};
        f();
        printf("mean = %f\n", tracker.mean);
        printf("max = %f\n", tracker.max);
        printf("min = %f\n", tracker.min);
        printf("variance = %f\n", tracker.variance());
        gc::GcHeap::destroy();
    };
    bench(gc::GcMode::STOP_THE_WORLD);
    bench(gc::GcMode::INCREMENTAL);
    bench(gc::GcMode::CONCURRENT);
}
void bench_random_graph_large() {
    printf("Running random graph benchmark (Large)\n");
    gc::enable_time_tracking = true;
    auto bench = [](gc::GcMode mode) {
        printf("benchmarking %s\n", gc::to_string(mode));
        gc::GcOption option{};
        option.mode = mode;
        option.max_heap_size = 1024 * 1024 * 256;
        gc::GcHeap::init(option);
        Rng rng(0);
        StatsTracker tracker;
        {
            std::vector<gc::Local<Node<int>>> presistent_nodes;
            for (auto j = 0; j < 40; j++) {
                auto time = std::chrono::high_resolution_clock::now();
                gc::Local<Node<int>> root = gc::Local<Node<int>>::make();
                auto n = 32768;
                for (int i = 0; i < n; i++) {
                    auto node = gc::Local<Node<int>>::make();
                    node->val = i;
                    root->children->push_back(node);
                }
                auto random_walk = [&](gc::GcPtr<Node<int>> node) -> gc::GcPtr<Node<int>> {
                    while (true) {
                        auto n_children = node->children->size();
                        if (n_children == 0) {
                            return node;
                        }
                        auto idx = rng.pcg32() % n_children;
                        node = node->children->at(idx);
                        auto terminate = rng.pcg32() % 10;
                        if (terminate == 0) {
                            return node;
                        }
                    }
                };
                for (auto i = 0; i < 65536; i++) {
                    auto node1 = random_walk(root);
                    auto node2 = random_walk(root);
                    node1->children->push_back(node2);
                }
                auto all_nodes = find_all_nodes(gc::GcPtr<Node<int>>(root));
                // printf("all_nodes.size() = %lld\n", all_nodes.size());
                auto sum = std::accumulate(all_nodes.begin(), all_nodes.end(), 0, [](int acc, auto node) {
                    return acc + node->val;
                });
                // printf("sum = %d\n", sum);
                GC_ASSERT(sum == (n - 1) * n / 2, "invalid sum");
                if (presistent_nodes.size() <= 10) {
                    presistent_nodes.push_back(root);
                } else {
                    auto idx = rng.pcg32() % presistent_nodes.size();
                    // remove idx by swapping with the last element
                    std::swap(presistent_nodes[idx], presistent_nodes.back());
                    presistent_nodes.pop_back();
                }
                auto elapsed = std::chrono::high_resolution_clock::now() - time;
                // printf("elapsed = %lldus\n", std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count());
                tracker.update(static_cast<double>(elapsed.count()) * 1e-6);
            }
        }
        printf("total time = %f\n", tracker.mean * tracker.count);
        tracker.print("bench random_graph");
        gc::get_heap().stats().print();
        gc::GcHeap::destroy();
    };
    bench(gc::GcMode::STOP_THE_WORLD);
    bench(gc::GcMode::INCREMENTAL);
    bench(gc::GcMode::CONCURRENT);
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
    bench_allocation();
    bench_allocation_collect();
    bench_random_graph_large();
    return 0;
}