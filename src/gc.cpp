#include "gc.h"
#include <optional>
namespace gc {
void TracingContext::shade(const GcObjectContainer *ptr) const noexcept {
    heap.shade(ptr);
}
static thread_local std::optional<GcHeap> heap = std::nullopt;
void GcHeap::init(GcOption option) {
    heap.emplace(option, gc_ctor_token_t{});
}
GcHeap &get_heap() {
    GC_ASSERT(heap.has_value(), "Heap is not initialized");
    return heap.value();
}

bool GcHeap::mark_some() {
    GC_ASSERT(state == State::MARKING, "State should be marking");
    if (work_list.empty()) {
        return false;
    }
    auto ptr = pop_from_working_list();
    scan(ptr);
    return true;
}
void GcHeap::scan_roots() {
    state = State::MARKING;
    if constexpr (is_debug) {
        std::printf("scanning %lld roots\n", root_set_.size());
    }
    for (auto root : root_set_) {
        GC_ASSERT(root->is_root(), "Root should be root");
        if constexpr (is_debug) {
            std::printf("scanning root %p\n", static_cast<const void *>(root));
        }
        scan(root);
    }
}
void GcHeap::sweep() {
    state = State::SWEEPING;
    if constexpr (is_debug) {
        std::println("starting sweep");
        auto obj_cnt = 0;
        auto root_cnt = 0;
        auto ptr = head_;
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
    auto ptr = head_;
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
                head_ = next;
            } else {
                prev->next_ = next;
            }
            ptr = next;
        }
    }
    state = State::IDLE;
}
void GcHeap::collect() {
    auto ptr = head_;
    while (ptr) {
        ptr->set_color(color::WHITE);
        ptr = ptr->next_;
    }
    state = State::MARKING;
    scan_roots();
    while (!work_list.empty()) {
        mark_some();
    }
    sweep();
}
}// namespace gc