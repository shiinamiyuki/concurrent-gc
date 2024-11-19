#include "gc.h"
#include <optional>
namespace gc {
void TracingContext::shade(const GcObjectContainer *ptr) const noexcept {
    heap.work_list.with([&](auto &wl, auto *lock) {
        heap.shade(ptr);
    });
}
static thread_local std::optional<GcHeap> heap = std::nullopt;
GcHeap::GcHeap(GcOption option, gc_ctor_token_t)
    : mode_(option.mode),
      max_heap_size_(option.max_heap_size),
      gc_threshold_(option.gc_threshold),
      pool_(detail::LockProtected<detail::spin_lock, Pool>::emplace_t{}, option.mode == GcMode::CONCURRENT, option),
      object_list_(ObjectList{}, option.mode == GcMode::CONCURRENT),
      root_set_(RootSet{}, option.mode == GcMode::CONCURRENT),
      work_list(WorkList{}, option.mode == GcMode::CONCURRENT),
      gc_memory_resource_(this) {
}
void GcHeap::concurrent_collector() {
    while (!stop_collector_.load(std::memory_order_relaxed)) {
        while (work_list.with([](auto &wl, auto *lock) {
            return wl.empty();
        })) {
            detail::pause_thread();
        }
        scan_roots();
        while (mark_some(10)) {}
        sweep();
    }
}
void GcHeap::init(GcOption option) {
    if (heap.has_value()) {
        std::fprintf(stderr, "Heap is already initialized\n");
        return std::abort();
    }
    heap.emplace(option, gc_ctor_token_t{});
    if (option.mode == GcMode::CONCURRENT) {
        heap->collector_thread_.emplace([&] {
            heap->concurrent_collector();
        });
    }
}
GcHeap &get_heap() {
    GC_ASSERT(heap.has_value(), "Heap is not initialized");
    return heap.value();
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
        }
        for (auto root : rs) {
            GC_ASSERT(root->is_root(), "Root should be root");
            if constexpr (is_debug) {
                std::printf("scanning root %p\n", static_cast<const void *>(root));
            }
            scan(root);
        }
    });
}
void GcHeap::sweep() {
    if (mode_ != GcMode::CONCURRENT) {
        state() = State::SWEEPING;
    }
    object_list_.with([&](auto &list, auto *lock) {
        if constexpr (is_debug) {
            std::println("starting sweep");
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
        GcObjectContainer *prev = nullptr;
        auto ptr = list.head;
        while (ptr) {
            auto next = ptr->next_;
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
                    list.head = next;
                } else {
                    prev->next_ = next;
                }
                ptr = next;
            }
        }
    });

    state() = State::IDLE;
}
void GcHeap::collect() {
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
}
}// namespace gc