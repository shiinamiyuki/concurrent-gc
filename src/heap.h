#include <list>
#include <memory_resource>
#include <malloc.h>
#include <cstdlib>
namespace gc {
struct RawHeap : std::pmr::memory_resource {
    struct FreeBlock {
        size_t size;
        uint8_t *ptr;
    };
    struct Block;
    struct PtrMetadata {
        Block *block{};
        uint16_t offset{};
        PtrMetadata(Block *block, uint16_t offset) : block(block), offset(offset) {}
    };
    struct Block {
        RawHeap *heap{};
        uint8_t *begin{};
        std::list<FreeBlock> free_blocks;

        Block(RawHeap *heap, uint8_t *begin, size_t size) : heap(heap), begin(begin + sizeof(Block)) {
            free_blocks.push_back({size - sizeof(Block), begin + sizeof(Block)});
        }

        uint8_t *allocate(size_t size, size_t align) {
            auto actual_size = size + sizeof(PtrMetadata);
            for (auto it = free_blocks.begin(); it != free_blocks.end(); ++it) {
                auto &block = *it;
                auto ptr = block.ptr;
                auto offset = align - (reinterpret_cast<size_t>(ptr) % align);
                if (block.size >= actual_size + offset) {
                    auto new_ptr = ptr + offset;
                    block.ptr = new_ptr + actual_size;
                    block.size -= actual_size + offset;
                    if (block.size == 0) {
                        free_blocks.erase(it);
                    }
                    auto metadata = PtrMetadata(this, offset);
                    std::memcpy(new_ptr + size, &metadata, sizeof(PtrMetadata));
                    return new_ptr;
                }
            }
            return nullptr;
        }
        void free(uint8_t *ptr, size_t size) {
            PtrMetadata metadata(this, 0);
            std::memcpy(ptr + size, &metadata, sizeof(PtrMetadata));
            free_blocks.push_back({size + sizeof(PtrMetadata) + metadata.offset, ptr - metadata.offset});
            std::printf("freeing %p, size = %lld\n", ptr - metadata.offset, size + sizeof(PtrMetadata) + metadata.offset);
        }
    };
    static constexpr size_t page_size = 4096;
    std::vector<std::list<Block *>> pooled_blocks;
    Block *allocate_block(size_t n_pages) {
#ifdef _WIN32
        auto ptr = static_cast<uint8_t *>(_aligned_malloc(n_pages * page_size, page_size));
#else
        auto ptr = static_cast<uint8_t *>(std::aligned_alloc(page_size, n_pages * page_size));
#endif
        new (ptr) Block(this, ptr, n_pages * page_size);
        return reinterpret_cast<Block *>(ptr);
    }
    void free_block(Block *block) {
        if (block->heap != this) {
            std::abort();
        }
        auto ptr = reinterpret_cast<uint8_t *>(block);
#ifdef _WIN32
        _aligned_free(ptr);
#else
        std::free(ptr);
#endif
    }

    size_t get_pool_index(size_t size) {
        if (size <= 128) {
            return 0;
        }
        if (size <= 256) {
            return 1;
        }
        if (size <= 512) {
            return 2;
        }
        if (size <= 1024) {
            return 3;
        }
        return 4;
    }
    void *do_allocate(std::size_t bytes, std::size_t alignment) override {
        auto pool_index = get_pool_index(bytes);
        auto &blocks = pooled_blocks[pool_index];
        Block *block = nullptr;
        for (auto b : blocks) {
            if (auto ptr = b->allocate(bytes, alignment)) {
                return ptr;
            }
        }
        block = allocate_block((bytes + sizeof(Block) + alignment - 1) / 4096 + 1);
        blocks.push_back(block);
        return block->allocate(bytes, alignment);
    }
    void do_deallocate(void *p, std::size_t bytes, std::size_t alignment) override {
        auto block = RawHeap::get_block_from_ptr(p, bytes);
        if (block->heap != this) {
            std::abort();
        }
        block->free(reinterpret_cast<uint8_t *>(p), bytes);
    }
    bool do_is_equal(const std::pmr::memory_resource &other) const noexcept override {
        return this == &other;
    }
    RawHeap() {
        pooled_blocks.resize(5);
    }
    ~RawHeap() {
        for (auto &blocks : pooled_blocks) {
            for (auto block : blocks) {
                free_block(block);
            }
        }
    }
    static Block *get_block_from_ptr(void *ptr, size_t size) {
        PtrMetadata meta(nullptr, 0);
        std::memcpy(&meta, static_cast<uint8_t *>(ptr) + size, sizeof(PtrMetadata));
        return meta.block;
    }
};
}// namespace gc