#include "gc.h"
#include <print>
namespace gc {

GcHeap &get_heap() {
    static thread_local GcHeap heap;
    return heap;
}
void GcHeap::collect() {
    auto *ptr = head_;
    for (; ptr != nullptr; ptr = ptr->next_) {
        ptr->color_ = Color::WHITE;
    };
    std::function<void(const GcObjectContainer*)> tracing_func;
    Tracer tracer{tracing_func};
    tracing_func = [&](const GcObjectContainer *ptr) {
        if(ptr->color_ != Color::WHITE) {
            return;
        }
        ptr->color_ = Color::GRAY;
        if (auto traceable = ptr->as_tracable(); traceable != nullptr) {
            traceable->trace(tracer);
        }
        ptr->color_ = Color::BLACK;
    };
    for (ptr = head_; ptr != nullptr; ptr = ptr->next_) {
        if (ptr->is_root_) {
            tracing_func(ptr);
        }
    }
    GcObjectContainer *prev = nullptr;
    ptr = head_;
    while (ptr) {
        auto next = ptr->next_;
        GC_ASSERT(ptr->color_ != Color::GRAY, "Object should not be gray");
        if (ptr->is_root_ || ptr->color_ == Color::BLACK) {
            prev = ptr;
            ptr = next;
        } else {
            std::printf("freeing object %p\n", static_cast<void*>(ptr));
            free_object(ptr);
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