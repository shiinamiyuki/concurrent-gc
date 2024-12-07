#include "gc.h"
#include <optional>
namespace gc {
bool enable_time_tracking = false;
void TracingContext::shade(const GcObjectContainer *ptr) const noexcept {
    // heap.work_list.with([&](auto &wl, auto *lock) {
    heap.shade(ptr, pool_idx);
    // });
}
void GcHeap::shade(const GcObjectContainer *ptr, size_t pool_idx) {
    if (mode_ != GcMode::CONCURRENT) {
        if (state() != State::MARKING) {
            return;
        }
    }
    if (!ptr || ptr->color() != color::WHITE)
        return;
    if (is_paralle_collection()) {
        uint8_t expected = color::WHITE;
        if (!ptr->color_.compare_exchange_strong(expected, color::GRAY)) {
            return;
        }
    } else {
        ptr->set_color(color::GRAY);
    }
    if (ptr->as_tracable()) {
        add_to_working_list(ptr, pool_idx);
    }
}
static std::shared_ptr<GcHeap> heap;
thread_local std::optional<size_t> tl_pool_idx;
GcHeap::GcHeap(GcOption option, gc_ctor_token_t)
    : mode_(option.mode),
      max_heap_size_(option.max_heap_size),
      gc_threshold_(option.gc_threshold),
      pool_(detail::emplace_t{}, option.mode == GcMode::CONCURRENT, option),
      object_lists_(ObjectLists{}, option.mode == GcMode::CONCURRENT),
      root_set_(RootSet{}, option.mode == GcMode::CONCURRENT),
      work_list(WorkList{}, option.mode == GcMode::CONCURRENT) {
    if (option.n_collector_threads.has_value()) {
        GC_ASSERT(option.mode != GcMode::INCREMENTAL, "Incremental mode does not support multiple threads");
        GC_ASSERT(option.n_collector_threads.value() > 0, "Number of collector threads should be positive");
        worker_pool_.emplace(option.n_collector_threads.value());
        worker_pool_->dispatch([&](size_t tid) {
            tl_pool_idx = tid;
        });
        auto &concurrent_resources = pool_.get().concurrent_resources;
        auto &lists = object_lists_.get().lists;
        for (auto i = 0; i < option.n_collector_threads.value(); i++) {
            lists.emplace_back(std::move(std::make_unique<ObjectLists::list_t>(ObjectList{}, true)));
            work_list.get().lists.emplace_back(std::move(std::make_unique<WorkList::list_t>(std::deque<const GcObjectContainer *>{}, false)));
            gc_memory_resource_.emplace_back(this, i);
        }

    } else {
        object_lists_.get().lists.emplace_back(std::move(std::make_unique<ObjectLists::list_t>(ObjectList{}, false)));
        work_list.get().lists.emplace_back(std::move(std::make_unique<WorkList::list_t>(std::deque<const GcObjectContainer *>{}, false)));
        gc_memory_resource_.emplace_back(this, 0);
    }
}
void GcHeap::signal_collection() {
    if constexpr (is_debug) {
        std::printf("Signaling collection\n");
    }
    pool_.get().concurrent_state = ConcurrentState::REQUESTED;
}
void GcHeap::do_concurrent(size_t inc_size) {
    pool_.with([&](auto &pool, auto *lock) {
        auto is_mem_available = [&]() {
            return pool.allocation_size_ + inc_size < max_heap_size_;
        };
        auto threshold = [&]() -> bool {
            auto time_threshold = [&]() {
                auto now = std::chrono::high_resolution_clock::now();
                auto since_last_collection = now - stats_.last_collect_time;
                return since_last_collection > std::chrono::seconds(1);
            };
            return pool.allocation_size_ + inc_size > max_heap_size_ * gc_threshold_ && (stats_.n_allocated - stats_.n_collected) >= stats_.last_collected && time_threshold();
        };
        stats_.wait_for_atomic_marking += time_function([&] {
            // while (pool.concurrent_state == ConcurrentState::ATOMIC_MARKING) {
            //     if constexpr (is_debug) {
            //         std::printf("Waiting for sweeping\n");
            //     }
            //     lock->unlock();
            //     detail::pause_thread();
            //     lock->lock();
            // }
            lock->wait([this, &pool] { return pool.concurrent_state == ConcurrentState::ATOMIC_MARKING; });
        });
        // in concurrent mode, the mutator might allocate too fast such that a single sweep is not enough
        // this is partly due to newly allocated objects are only collected in the next sweep
        auto n_retry = 2;
        for (auto r = 0; r < n_retry; r++) {
            if (!is_mem_available() || threshold()) {
                if (pool.concurrent_state == ConcurrentState::IDLE) {
                    // std::printf("stats_.n_allocated = %lld\n stats_.n_collected = %lld\n stats_.last_collected = %lld\n", stats_.n_allocated.load(), stats_.n_collected.load(), stats_.last_collected);
                    signal_collection();
                }

                if (!is_mem_available()) {
                    GC_ASSERT(pool.concurrent_state != ConcurrentState::IDLE, "State should not be idle");
                    if (pool.concurrent_state == ConcurrentState::REQUESTED || pool.concurrent_state == ConcurrentState::MARKING) {
                        stats_.wait_for_atomic_marking += time_function([&] {
                            // while (pool.concurrent_state == ConcurrentState::REQUESTED || pool.concurrent_state == ConcurrentState::MARKING) {
                            //     if constexpr (is_debug) {
                            //         std::printf("Memory not enough, waiting for collection\n");
                            //     }
                            //     lock->unlock();
                            //     detail::pause_thread();
                            //     lock->lock();
                            // }
                            lock->wait([this, &pool] {
                                return pool.concurrent_state == ConcurrentState::REQUESTED || pool.concurrent_state == ConcurrentState::MARKING;
                            });
                        });
                    }
                    stats_.wait_for_atomic_marking += time_function([&] {
                        // while (pool.concurrent_state == ConcurrentState::ATOMIC_MARKING) {
                        //     if constexpr (is_debug) {
                        //         std::printf("Waiting for sweeping\n");
                        //     }
                        //     lock->unlock();
                        //     detail::pause_thread();
                        //     lock->lock();
                        // }
                        lock->wait([this, &pool] { return pool.concurrent_state == ConcurrentState::ATOMIC_MARKING; });
                        // while (pool.concurrent_state != ConcurrentState::IDLE) {
                        //     lock->unlock();
                        //     detail::pause_thread();
                        //     lock->lock();
                        // }
                        lock->wait([this, &pool] { return pool.concurrent_state != ConcurrentState::IDLE; });
                    });
                }
                if (is_mem_available()) {
                    return;
                }
            }
        }
        GC_ASSERT(is_mem_available(), "Out of memory");
    },
               false, true);
}
void GcHeap::concurrent_collector() {
    while (!stop_collector_.load(std::memory_order_relaxed)) {
        pool_.with([&](Pool &pool, auto *lock) {
            // while (pool.concurrent_state == ConcurrentState::IDLE && !stop_collector_.load(std::memory_order_relaxed)) {
            //     lock->unlock();
            //     detail::pause_thread();
            //     lock->lock();
            // }
            lock->wait([this, &pool] {
                return pool.concurrent_state == ConcurrentState::IDLE && !stop_collector_.load(std::memory_order_relaxed);
            });
            if (stop_collector_.load(std::memory_order_relaxed)) {
                return;
            }
            // std::printf("state = %d\n", pool.concurrent_state);
            GC_ASSERT(pool.concurrent_state == ConcurrentState::REQUESTED, "Mutator should request collection");
            pool.concurrent_state = ConcurrentState::MARKING;
            if constexpr (is_debug) {
                std::printf("Starting concurrent collection\n");
            }
        });
        if (stop_collector_.load(std::memory_order_relaxed)) {
            return;
        }

        auto t = time_function([&]() {
            scan_roots();
            // marking only acquire lock on the work list
            // but not the pool. at this time, the mutator can still allocate and push stuff to the pool
            // what do we do?
            if (is_paralle_collection()) {
                parallel_marking();
            } else {
                while (mark_some(10)) {}
            }
            pool_.with([&](Pool &pool, auto *lock) {
                GC_ASSERT(pool.concurrent_state == ConcurrentState::MARKING, "State should be marking");
                pool.concurrent_state = ConcurrentState::ATOMIC_MARKING;

                if constexpr (is_debug) {
                    std::printf("Concurrent sweeping\n");
                }

                if (is_paralle_collection()) {
                    parallel_marking();
                } else {
                    while (mark_some(0xff)) {}
                }
            });
            sweep();
            pool_.with([&](Pool &pool, auto *lock) {
                pool.concurrent_state = ConcurrentState::IDLE;
                if constexpr (is_debug) {
                    std::printf("Concurrent collection done\n");
                }
            });
        });
        stats_.collection_time.update(t);
        stats_.n_collection_cycles.fetch_add(1, std::memory_order_relaxed);
    }
}
void GcHeap::init(GcOption option) {
    if (heap) {
        std::fprintf(stderr, "Heap is already initialized\n");
        return std::abort();
    }
    heap = std::make_shared<GcHeap>(option, gc_ctor_token_t{});
    if (option.mode == GcMode::CONCURRENT) {
        heap->collector_thread_.emplace([&] {
            heap->concurrent_collector();
        });
    }
}
void GcHeap::destroy() {
    if (heap) {
        heap->stop();
        heap.reset();
    }
}
GcHeap &get_heap() {
    GC_ASSERT(heap != nullptr, "Heap is not initialized");
    return *heap;
}
void GcHeap::parallel_marking() {
    GC_ASSERT(worker_pool_.has_value(), "Worker pool should be initialized");
    auto &workers = worker_pool_.value();
    work_list.with([&](auto &wl, auto *lock) {
        auto mark = [&](size_t pool_idx) {
            if constexpr (is_debug) {
                std::printf("Worker %lld started\n", pool_idx);
            }
            auto cnt = 0;
            wl.lists.at(pool_idx)->with([&](auto &list, auto *lock) {
                // auto&list = wl.lists.at(pool_idx)->get();
                while (!list.empty()) {
                    if constexpr (is_debug) {
                        std::printf("Worker %lld has %lld items\n", pool_idx, list.size());
                    }
                    auto ptr = list.front();
                    list.pop_front();
                    scan(ptr, pool_idx);
                    cnt++;
                }
            });
            // std::printf("Worker %lld processed %lld items\n", pool_idx, cnt);
        };
        // auto t0 =std::chrono::high_resolution_clock::now();
        workers.dispatch(mark);
        // auto t1 = std::chrono::high_resolution_clock::now();
        // auto t = (t1 - t0).count();
        // std::printf("Parallel marking took %f s\n", t*1e-9);

        // for (auto i = 0; i < wl.lists.size(); i++) {
        //     mark(i);
        // }
    });
}
bool GcHeap::mark_some(size_t max_count) {
    GC_ASSERT(!is_paralle_collection(), "Should not called in parallel collection");
    if (mode_ != GcMode::CONCURRENT) {
        GC_ASSERT(state() == State::MARKING, "State should be marking");
    }
    return work_list.with([&](auto &wl, auto *lock) {
        std::vector<const GcObjectContainer *> to_scan;
        for (auto i = 0; i < max_count; i++) {
            if (wl.empty()) {
                return false;
            }
            auto ptr = pop_from_working_list();
            scan(ptr, 0);
        }
        return true;
    });
}
void GcHeap::scan_roots() {
    if (mode_ != GcMode::CONCURRENT) {
        state() = State::MARKING;
    }
    root_set_.with([&](RootSet &rs, auto *lock) {
        if constexpr (is_debug) {
            std::printf("scanning %lld roots\n", rs.size());
            std::fflush(stdout);
        }
        for (auto root : rs) {
            // if (mode() != GcMode::CONCURRENT) {
            //     // in concurrent mode, root can remove itself from the root set while being scanned
            //     GC_ASSERT(root->is_root(), "Root should be root");
            // }
            GC_ASSERT(root->is_root(), "Root should be root");
            if constexpr (is_debug) {
                std::printf("scanning root %p\n", static_cast<const void *>(root));
                std::fflush(stdout);
            }

            work_list.with([&](auto &wl, auto *lock) {
                scan(root, wl.least_filled());
            });
        }
    });
}
std::tuple<size_t, size_t, GcObjectContainer *, GcObjectContainer *> GcHeap::sweep_list(ObjectList &object_list, size_t pool_idx) {
    if constexpr (is_debug) {
        std::printf("sweeping list %p\n", static_cast<void *>(object_list.head));
        std::printf("starting sweep, pool_idx = %lld\n", pool_idx);
        auto obj_cnt = 0;
        auto root_cnt = 0;
        auto reacheable_cnt = 0;
        auto ptr = object_list.head;
        while (ptr) {
            obj_cnt++;
            std::printf("ptr is %p,  pool_idx = %lld\n", static_cast<void *>(ptr), pool_idx);
            if (ptr->is_root()) {
                root_cnt++;
                reacheable_cnt++;
            } else if (ptr->color() == color::BLACK) {
                reacheable_cnt++;
            }
            ptr = ptr->next_;
        }
        std::printf("sweeping %d objects, %d roots, %d reacheable\n", obj_cnt, root_cnt, reacheable_cnt);
    }
    GcObjectContainer *head = object_list.head;
    GcObjectContainer *ptr = object_list.head;
    GcObjectContainer *prev = nullptr;
    auto cnt = 0ull;
    auto collect_cnt = 0ull;
    auto object_ist_cnt = object_list.count.load();
    auto collected_bytes = 0ull;
    while (ptr) {
        auto next = ptr->next_;
        // if (mode() != GcMode::CONCURRENT) {
        //     GC_ASSERT(ptr->color() != color::GRAY, "Object should not be gray");
        // }
        GC_ASSERT(ptr->color() != color::GRAY, "Object should not be gray");
        if (ptr->is_root()) {
            GC_ASSERT(ptr->color() == color::BLACK, "Root should be black");
        }
        if (ptr->color() == color::BLACK) {
            prev = ptr;
            ptr->set_color(color::WHITE);
            ptr = next;
        } else {
            collected_bytes += ptr->object_size();
            free_object(ptr, pool_idx);

            // object_list.count.fetch_sub(1, std::memory_order_relaxed);
            object_ist_cnt -= 1;
            // GC_ASSERT(!ptr->is_alive(), "Object should not be alive");
            if (!prev) {
                head = next;
            } else {
                prev->next_ = next;
            }
            ptr = next;
            collect_cnt++;
        }
        cnt++;
    }
    stats_.n_collected.fetch_add(collect_cnt, std::memory_order_relaxed);
    pool_.get().allocation_size_.fetch_sub(collected_bytes, std::memory_order_relaxed);
    object_list.count.store(object_ist_cnt, std::memory_order_relaxed);
    // std::printf("sweeped %d objects, %d collected from pool %lld\n", cnt, collect_cnt, pool_idx);
    return {collect_cnt, cnt, head, prev};
}
void GcHeap::sweep() {
    if (mode_ != GcMode::CONCURRENT) {
        state() = State::SWEEPING;
    }
    auto t = time_function([&] {
        // GcObjectContainer *head = nullptr;
        // GcObjectContainer *ptr = nullptr;
        // GcObjectContainer *prev = nullptr;
        std::vector<ObjectList> object_lists;
        object_lists_.with([&](auto &lists, auto *lock) {
            for (auto &list : lists.lists) {
                list->with([&](ObjectList &list, auto *lock) {
                    if constexpr (is_debug) {
                        std::printf("starting sweep\n");
                        auto obj_cnt = 0;
                        auto root_cnt = 0;
                        auto ptr = list.head;
                        while (ptr) {
                            obj_cnt++;
                            if (ptr->is_root()) {
                                root_cnt++;
                            }
                            ptr = ptr->next_;
                        }
                        std::printf("sweeping %d objects, %d roots\n", obj_cnt, root_cnt);
                    }
                    object_lists.emplace_back(list);
                    list.head = nullptr;
                });
            }
        });
        pool_.with([&](auto &pool, auto *lock) {
            pool.concurrent_state = ConcurrentState::SWEEPING;
        });
        auto do_sweep = [&](size_t i) {
            GC_ASSERT(object_lists.size() == object_lists_.get().lists.size(), "Size should be the same");
            auto t0 = std::chrono::high_resolution_clock::now();
            auto tmp_list = object_lists[i];
            auto [collect_cnt, cnt, head, prev] = sweep_list(tmp_list, i);
            stats_.n_collected.fetch_add(collect_cnt, std::memory_order_relaxed);
            object_lists_.get().lists[i]->with([&](auto &list, auto *lock) {
                if (prev) {
                    GC_ASSERT(head != nullptr, "Head should not be null");
                    prev->next_ = list.head;
                    list.head = head;
                    list.count.store(tmp_list.count.load(), std::memory_order_relaxed);
                }
            });
            auto t1 = std::chrono::high_resolution_clock::now();
            auto t = (t1 - t0).count();
            // if constexpr (is_debug) {
            // std::printf("Sweeping list %lld took %f s\n", i, t * 1e-9);
            // }
        };
        if (is_paralle_collection()) {
            auto &workers = worker_pool_.value();
            auto t0 = std::chrono::high_resolution_clock::now();
            workers.dispatch(do_sweep);
            auto t1 = std::chrono::high_resolution_clock::now();
            auto t = (t1 - t0).count();
            // if constexpr (is_debug) {
            // std::printf("Parallel sweeping took %f s\n", t * 1e-9);
            // }
        } else {
            auto t0 = std::chrono::high_resolution_clock::now();
            for (int i = 0; i < object_lists.size(); i++) {
                do_sweep(i);
            }
            auto t1 = std::chrono::high_resolution_clock::now();
            auto t = (t1 - t0).count();
            // if constexpr (is_debug) {
                // std::printf("sweeping took %f s\n", t * 1e-9);
            // }
        }

        if (mode_ != GcMode::CONCURRENT) {
            state() = State::IDLE;
        }
    });
    stats_.sweep_time.update(t);
    stats_.last_collect_time = std::chrono::high_resolution_clock::now();
}
void GcHeap::collect() {

    auto t = time_function([&]() {
        if constexpr (is_debug) {
            std::printf("starting full collection\n");
        }
        // auto object_list = object_list_.get();
        // auto ptr = object_list.head;
        // while (ptr) {
        //     ptr->set_color(color::WHITE);
        //     ptr = ptr->next_;
        // }
        object_lists_.with([&](auto &lists, auto *lock) {
            for (auto &list : lists.lists) {
                list->with([&](auto &list, auto *lock) {
                    auto ptr = list.head;
                    while (ptr) {
                        ptr->set_color(color::WHITE);
                        ptr = ptr->next_;
                    }
                });
            }
        });
        work_list.get().clear();
        if (mode_ != GcMode::CONCURRENT) {
            state() = State::MARKING;
        }
        scan_roots();
        if (is_paralle_collection()) {
            parallel_marking();
        } else {
            while (mark_some(10)) {}
        }
        GC_ASSERT(work_list.get().empty(), "Work list should be empty");
        sweep();
        if constexpr (is_debug) {
            std::printf("full collection done\n");
        }
    });
    if (stats_.incremental_time > 0.0) {
        t += stats_.incremental_time;
    }
    stats_.collection_time.update(t);
    stats_.n_collection_cycles.fetch_add(1, std::memory_order_relaxed);
    stats_.incremental_time = 0;
}
}// namespace gc