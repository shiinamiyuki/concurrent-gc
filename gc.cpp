#include "gc.h"
#include <print>
namespace gc {

GcHeap &get_heap() {
    static thread_local GcHeap heap;
    return heap;
}
void GcHeap::collect() {
    auto *ptr = head_;
    std::function<void(const GcObjectContainer *)> tracing_func;
    Tracer tracer{tracing_func};
    tracing_func = [&](const GcObjectContainer *ptr) {
        if (ptr->color() != color::WHITE) {
            return;
        }
        ptr->set_color(color::GRAY);
        if (auto traceable = ptr->as_tracable(); traceable != nullptr) {
            traceable->trace(tracer);
        }
        ptr->set_color(color::BLACK);
    };
    for (ptr = head_; ptr != nullptr; ptr = ptr->next_) {
        if (ptr->is_root()) {
            tracing_func(ptr);
        }
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