#include "gc.h"
#include <optional>
namespace gc {
bool enable_time_tracking = false;
void TracingContext::shade(const GcObjectContainer *ptr) const noexcept {
    // heap.work_list.with([&](auto &wl, auto *lock) {
    heap.shade(ptr);
    // });
}
void GcHeap::shade(const GcObjectContainer *ptr) {
    if (mode_ != GcMode::CONCURRENT) {
        if (state() != State::MARKING) {
            return;
        }
    }
    if (!ptr || ptr->color() != color::WHITE)
        return;
    ptr->set_color(color::GRAY);
    if (ptr->as_tracable()) {
        add_to_working_list(ptr);
    }
}
static std::shared_ptr<GcHeap> heap;
GcHeap::GcHeap(GcOption option, gc_ctor_token_t)
    : mode_(option.mode),
      max_heap_size_(option.max_heap_size),
      gc_threshold_(option.gc_threshold),
      pool_(detail::emplace_t{}, option.mode == GcMode::CONCURRENT, option),
      object_list_(ObjectList{}, option.mode == GcMode::CONCURRENT),
      root_set_(RootSet{}, option.mode == GcMode::CONCURRENT),
      work_list(WorkList{}, option.mode == GcMode::CONCURRENT),
      gc_memory_resource_(this) {
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
            while (pool.concurrent_state == ConcurrentState::ATOMIC_MARKING) {
                if constexpr (is_debug) {
                    std::printf("Waiting for sweeping\n");
                }
                lock->unlock();
                detail::pause_thread();
                lock->lock();
            }
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
                        while (pool.concurrent_state == ConcurrentState::REQUESTED || pool.concurrent_state == ConcurrentState::MARKING) {
                            if constexpr (is_debug) {
                                std::printf("Memory not enough, waiting for collection\n");
                            }
                            lock->unlock();
                            detail::pause_thread();
                            lock->lock();
                        } });
                    }
                    stats_.wait_for_atomic_marking += time_function([&] {
                        while (pool.concurrent_state == ConcurrentState::ATOMIC_MARKING) {
                            if constexpr (is_debug) {
                                std::printf("Waiting for sweeping\n");
                            }
                            lock->unlock();
                            detail::pause_thread();
                            lock->lock();
                        }
                        while (pool.concurrent_state != ConcurrentState::IDLE) {
                            lock->unlock();
                            detail::pause_thread();
                            lock->lock();
                        }
                    });
                }
                if (is_mem_available()) {
                    return;
                }
            }
        }
        GC_ASSERT(is_mem_available(), "Out of memory");
    });
}
void GcHeap::concurrent_collector() {
    while (!stop_collector_.load(std::memory_order_relaxed)) {
        pool_.with([&](Pool &pool, auto *lock) {
            while (pool.concurrent_state == ConcurrentState::IDLE && !stop_collector_.load(std::memory_order_relaxed)) {
                lock->unlock();
                detail::pause_thread();
                lock->lock();
            }
            if (stop_collector_.load(std::memory_order_relaxed)) {
                return;
            }
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
            while (mark_some(10)) {}
            pool_.with([&](Pool &pool, auto *lock) {
                GC_ASSERT(pool.concurrent_state == ConcurrentState::MARKING, "State should be marking");
                pool.concurrent_state = ConcurrentState::ATOMIC_MARKING;

                if constexpr (is_debug) {
                    std::printf("Concurrent sweeping\n");
                }

                while (mark_some(0xffff)) {}
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

bool GcHeap::mark_some(size_t max_count) {
    if (mode_ != GcMode::CONCURRENT) {
        GC_ASSERT(state() == State::MARKING, "State should be marking");
    }
    return work_list.with([&](auto &wl, auto *lock) {
        for (auto i = 0; i < max_count; i++) {
            if (wl.empty()) {
                return false;
            }
            auto ptr = pop_from_working_list();
            scan(ptr);
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
                scan(root);
            });
        }
    });
}
void GcHeap::sweep() {
    if (mode_ != GcMode::CONCURRENT) {
        state() = State::SWEEPING;
    }
    auto t = time_function([&] {
        GcObjectContainer *head = nullptr;
        GcObjectContainer *ptr = nullptr;
        GcObjectContainer *prev = nullptr;
        object_list_.with([&](auto &list, auto *lock) {
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
            head = list.head;
            ptr = list.head;
            list.head = nullptr;
        });
        pool_.with([&](auto &pool, auto *lock) {
            pool.concurrent_state = ConcurrentState::SWEEPING;
        });
        auto cnt = 0ull;
        auto collect_cnt = 0ull;
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
                free_object(ptr);
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
        stats_.last_collected = collect_cnt;
        stats_.ratio_collected.update(static_cast<double>(collect_cnt) / cnt);
        // });

        object_list_.with([&](auto &list, auto *lock) {
            if (prev) {
                GC_ASSERT(head != nullptr, "Head should not be null");
                prev->next_ = list.head;
                list.head = head;
            }
        });
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
        auto object_list = object_list_.get();
        auto ptr = object_list.head;
        while (ptr) {
            ptr->set_color(color::WHITE);
            ptr = ptr->next_;
        }
        work_list.get().clear();
        if (mode_ != GcMode::CONCURRENT) {
            state() = State::MARKING;
        }
        scan_roots();
        while (!work_list.get().empty()) {
            mark_some(10);
        }
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