
#include "gc.h"
#include "rc.h"
#include <array>
#include <numeric>
#include <chrono>
#include "test_common.h"
using StatsTracker = gc::StatsTracker;
struct Bar : gc::GarbageCollected<Bar> {
    int val;
    explicit Bar(int val = 0) : val(val) {}
    GC_CLASS()
};

struct Foo : gc::GarbageCollected<Foo> {
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
template<typename C, typename T>
struct Node : public C::template Enable<Node<C, T>> {
    // IMPORT_TYPES()
    T val{};
    C::template Member<typename C::template Array<Node<C, T>>> children;
    Node() : children(this) {
        children = C::template make<typename C::template Array<Node<C, T>>>();
    }
    GC_CLASS(children)
};
template<typename C, typename T>
auto find_all_nodes(typename C::template Ptr<Node<C, T>> root) {
    std::vector<typename C::template Ptr<Node<C, T>>> nodes;
    std::unordered_set<typename C::template Ptr<Node<C, T>>>
        visited;
    using N = C::template Ptr<Node<C, T>>;
    std::function<void(N)> dfs = [&](C::template Ptr<Node<C, T>> node) {
        if (visited.contains(node)) {
            return;
        }
        visited.insert(node);
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

void bench_short_lived_few_update() {
    printf("Running bench_short_lived_few_update\n");
    auto bench = []<class C>(C policy) {
        // gc::GcOption option{};
        // option.mode = mode;
        // option.max_heap_size = 1024 * 64;
        // gc::GcHeap::init(option);

        policy.init();
        StatsTracker tracker;
        auto f = [&] {
            for (auto j = 0; j < 400; j++) {
                auto root = C::template make<Node<C, int>>();
                auto time = std::chrono::high_resolution_clock::now();
                auto n = 100;
                for (int i = 0; i < n; i++) {
                    auto node = C::template make<Node<C, int>>();
                    node->val = i;

                    root->children->push_back(node);
                }
                int sum = 0;
                for (int i = 0; i < n; i++) {
                    sum += root->children->at(i)->val;
                    // std::printf("i=%d,node->val=%d\n", i, root->children->at(i)->val);
                    //   std::printf("%lld %p\n",root->children->at(i).control_block_->ref_count, static_cast<void *>(root->children->at(i).control_block_));
                }
                // gc::get_heap().collect();
                // std::printf("sum = %d\n", sum);
                GC_ASSERT(sum == n * (n - 1) / 2, "invalid sum");

                // std::printf("%lld %p\n", root.control_block_->ref_count, static_cast<void *>(root.control_block_));

                auto elapsed = std::chrono::high_resolution_clock::now() - time;
                tracker.update(static_cast<double>(elapsed.count()) * 1e-3);
                // std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        };
        printf("benchmarking %s\n", policy.name().c_str());
        f();
        tracker = StatsTracker{};
        f();
        printf("mean = %f\n", tracker.mean);
        printf("max = %f\n", tracker.max);
        printf("min = %f\n", tracker.min);
        printf("variance = %f\n", tracker.variance());
        // gc::GcHeap::destroy();
        policy.finalize();
    };
    bench(RcPolicy<rc::RefCounter>{});
    bench(RcPolicy<rc::AtomicRefCounter>{});
    gc::GcOption option{};
    option.max_heap_size = 1024 * 32;
    option.mode = gc::GcMode::STOP_THE_WORLD;
    bench(GcPolicy{option});
    option.mode = gc::GcMode::INCREMENTAL;
    bench(GcPolicy{option});
    option.mode = gc::GcMode::CONCURRENT;
    bench(GcPolicy{option});
    option.n_collector_threads = 4;
    option.mode = gc::GcMode::STOP_THE_WORLD;
    bench(GcPolicy{option});
    option.mode = gc::GcMode::CONCURRENT;
    bench(GcPolicy{option});
}

void bench_short_lived_frequent_update() {
    printf("Running bench_short_lived_frequent_update\n");
    auto bench = []<class C>(C policy) {
        // gc::GcOption option{};
        // option.mode = mode;
        // option.max_heap_size = 1024 * 64;
        // gc::GcHeap::init(option);
        Rng rng(0);
        policy.init();
        StatsTracker tracker;
        auto f = [&] {
            for (auto j = 0; j < 400; j++) {
                auto n = 100;
                auto root = C::template make<Node<C, int>>();
                auto time = std::chrono::high_resolution_clock::now();
                for (int i = 0; i < n; i++) {
                    auto node = C::template make<Node<C, int>>();
                    node->val = i;

                    root->children->push_back(node);
                }
                for (int i = 0; i < n; i++) {
                    auto i0 = rng.pcg32() % n;
                    auto i1 = rng.pcg32() % n;
                    if (i0 == i1) {
                        continue;
                    }
                    typename C::template Owned<Node<C, int>> tmp = root->children->at(i0);
                    root->children->at(i0) = root->children->at(i1);
                    root->children->at(i1) = tmp;
                }
                int sum = 0;
                for (int i = 0; i < n; i++) {
                    sum += root->children->at(i)->val;
                    // std::printf("i=%d,node->val=%d\n", i, root->children->at(i)->val);
                    //   std::printf("%lld %p\n",root->children->at(i).control_block_->ref_count, static_cast<void *>(root->children->at(i).control_block_));
                }
                // std::printf("sum = %d\n", sum);
                GC_ASSERT(sum == (n - 1) * n / 2, "invalid sum");

                // std::printf("%lld %p\n", root.control_block_->ref_count, static_cast<void *>(root.control_block_));

                auto elapsed = std::chrono::high_resolution_clock::now() - time;
                tracker.update(static_cast<double>(elapsed.count()) * 1e-3);
                // std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        };
        printf("benchmarking %s\n", policy.name().c_str());
        f();
        tracker = StatsTracker{};
        f();
        printf("mean = %f\n", tracker.mean);
        printf("max = %f\n", tracker.max);
        printf("min = %f\n", tracker.min);
        printf("variance = %f\n", tracker.variance());
        // gc::GcHeap::destroy();
        policy.finalize();
    };
    bench(RcPolicy<rc::RefCounter>{});
    bench(RcPolicy<rc::AtomicRefCounter>{});
    gc::GcOption option{};
    option.max_heap_size = 1024 * 16;
    option.mode = gc::GcMode::STOP_THE_WORLD;
    bench(GcPolicy{option});
    option.mode = gc::GcMode::INCREMENTAL;
    bench(GcPolicy{option});
    option.mode = gc::GcMode::CONCURRENT;
    bench(GcPolicy{option});
    option.n_collector_threads = 4;
    option.mode = gc::GcMode::STOP_THE_WORLD;
    bench(GcPolicy{option});
    option.mode = gc::GcMode::CONCURRENT;
    bench(GcPolicy{option});
}

void bench_random_graph_large() {
    printf("Running random graph benchmark (Large)\n");
    gc::enable_time_tracking = false;
    auto bench = [](gc::GcMode mode, bool parallel) {
        gc::GcOption option{};
        option.mode = mode;
        option.max_heap_size = 1024 * 1024 * 256;
        if (parallel) {
            option.n_collector_threads = 8;
        }
        printf("benchmarking %s\n", GcPolicy{option}.name().c_str());
        gc::GcHeap::init(option);
        Rng rng(0);
        StatsTracker tracker;
        {
            std::vector<gc::Local<Node<GcPolicy, int>>> presistent_nodes;
            for (auto j = 0; j < 100; j++) {
                auto time = std::chrono::high_resolution_clock::now();
                gc::Local<Node<GcPolicy, int>> root = gc::Local<Node<GcPolicy, int>>::make();
                auto n = 32768;
                for (int i = 0; i < n; i++) {
                    auto node = gc::Local<Node<GcPolicy, int>>::make();
                    node->val = i;
                    root->children->push_back(node);
                }
                auto random_walk = [&](gc::GcPtr<Node<GcPolicy, int>> node) -> gc::GcPtr<Node<GcPolicy, int>> {
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
                auto all_nodes = find_all_nodes<GcPolicy>(gc::GcPtr<Node<GcPolicy, int>>(root));
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
        if (gc::enable_time_tracking) {
            gc::get_heap().stats().print();
        }
        gc::GcHeap::destroy();
    };
    // bench(gc::GcMode::STOP_THE_WORLD, false);
    // bench(gc::GcMode::INCREMENTAL, false);
    // bench(gc::GcMode::CONCURRENT, false);
    bench(gc::GcMode::STOP_THE_WORLD, true);
    bench(gc::GcMode::CONCURRENT, true);
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
void test_concurrent_gc_multithread() {
    gc::GcOption option{};
    option.mode = gc::GcMode::CONCURRENT;
    option.max_heap_size = 1024 * 1024 * 64;
    gc::GcHeap::init(option);
    {
        std::vector<std::thread> threads;
        using NodeT = Node<GcPolicy, int>;
        auto shared_root = gc::Local<NodeT>::make();
        auto m = 8192;
        for (int i = 0; i < m; i++) {
            auto node = gc::Local<NodeT>::make();
            node->val = i;
            shared_root->children->push_back(node);
        }
        for (auto i = 0; i < 4; i++) {
            threads.emplace_back([i, shared_root, m] {
                Rng rng(i);
                for (auto j = 0; j < 25; j++) {
                    auto root = gc::Local<NodeT>::make();
                    auto n = 8192;
                    for (int i = 0; i < n; i++) {
                        auto node = gc::Local<NodeT>::make();
                        node->val = i;
                        root->children->push_back(node);
                    }
                    auto random_walk = [&](gc::GcPtr<NodeT> node) -> gc::GcPtr<NodeT> {
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
                    root->children->push_back(shared_root);
                    auto all_nodes = find_all_nodes<GcPolicy>(gc::GcPtr<NodeT>(root));
                    auto sum = std::accumulate(all_nodes.begin(), all_nodes.end(), 0, [](int acc, auto node) {
                        return acc + node->val;
                    });
                    GC_ASSERT(sum == (n - 1) * n / 2 + (m - 1) * m / 2, "invalid sum");
                }
            });
        }
        for (auto &t : threads) {
            t.join();
        }
    }
    std::printf("multithread test done\n");
    gc::GcHeap::destroy();
}
void test_hashmap() {
    gc::GcOption option{};
    option.mode = gc::GcMode::STOP_THE_WORLD;
    option.max_heap_size = 1024 * 1024 * 256;
    gc::GcHeap::init(option);
    using Map = gc::GcHashMap<gc::Adaptor<std::string>, gc::Adaptor<std::string>>;
    {
        auto map = gc::Local<Map>::make();
        for (int i = 0; i < 100; i++) {
            auto s = gc::Local<gc::Adaptor<std::string>>::make(std::to_string(i));
            map->insert(s, s);
            GC_ASSERT(map->contains(s), "should contain");
        }
        printf("map.size() = %lld\n", map->size());
        for (auto [k, v] : *map) {
            printf("%s %s\n", k->c_str(), v->c_str());
        }
    }

    gc::GcHeap::destroy();
}
int main() {
    // bench_short_lived_few_update();
    // bench_short_lived_frequent_update();
    bench_random_graph_large();
    // test_concurrent_gc_multithread();
    // test_hashmap();
    // gc::ThreadPool pool(4);
    // for (int i = 0; i < 20; i++) {
    //     std::atomic<int> counter = 0;
    //     pool.dispatch([&](size_t idx) {
    //         std::printf("hello from thread %lld\n", idx);
    //         std::fflush(stdout);
    //         counter++;
    //     });
    //     std::printf("dispatched %d, counter = %d\n", i, counter.load());
    // }
    return 0;
}