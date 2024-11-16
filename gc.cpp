#include "gc.h"
#include <print>
#include <deque>
namespace gc {

GcHeap &get_heap() {
    static thread_local GcHeap heap;
    return heap;
}
void GcHeap::mark_some() {
}
void GcHeap::collect() {
    auto *ptr = head_;
    // std::function<void(const GcObjectContainer *)> tracing_func;
    // Tracer tracer{tracing_func};
    std::deque<const GcObjectContainer *> work_list;
    auto tracing_ctx = TracingContext{std::vector<const GcObjectContainer *>{}};
    auto tracing_func = [&](const GcObjectContainer *ptr) {
        if (ptr->color() == color::BLACK) {
            return;
        }
        if (ptr->color() == color::GRAY) {
            ptr->set_color(color::BLACK);
            return;
        }
        GC_ASSERT(ptr->color() == color::WHITE, "Object should be white");
        ptr->set_color(color::GRAY);
        tracing_ctx.work_list.clear();
        if (auto tracable = ptr->as_tracable()) {
            tracable->trace(Tracer{tracing_ctx});
        }
        for (auto *child : tracing_ctx.work_list) {
            work_list.push_back(child);
        }
        work_list.push_back(ptr);
    };

    for (ptr = head_; ptr != nullptr; ptr = ptr->next_) {
        if (ptr->is_root()) {
            tracing_func(ptr);
        }
    }
    while (!work_list.empty()) {
        auto *ptr = work_list.front();
        work_list.pop_front();
        tracing_func(ptr);
    }
    GcObjectContainer *prev = nullptr;
    ptr = head_;
    while (ptr) {
        auto next = ptr->next_;
        GC_ASSERT(ptr->color() != color::GRAY, "Object should not be gray");
        if (ptr->is_root() || ptr->color() == color::BLACK) {
            prev = ptr;
            ptr->set_color(color::WHITE);
            ptr = next;
        } else {
            if constexpr (is_debug) {
                std::printf("freeing object %p\n", static_cast<void *>(ptr));
            }
            free_object(ptr);
            GC_ASSERT(!ptr->is_alive(), "Object should not be alive");
            if (!prev) {
                head_ = next;
            } else {
                prev->next_ = next;
            }
            ptr = next;
        }
    }
}
}// namespace gc