#pragma once

#include <cstddef>
#include <cstdint>

namespace mm
{
    constexpr size_t kFsaClassCount = 6;
    constexpr size_t kFsaMinBlock = 16;
    constexpr size_t kFsaMaxBlock = 512;
    constexpr size_t kFsaPageSize = 64 * 1024;
    constexpr size_t kFsaRegionSize = 64 * 1024 * 1024;
    constexpr size_t kLargeThreshold = 10 * 1024 * 1024;

    namespace detail
    {
        struct alignas(16) Block
        {
            size_t size;
            size_t prevSize;
        };

        struct FreeLinks
        {
            Block* next;
            Block* prev;
        };

        struct Segment
        {
            Segment* next;
            char* base;
            size_t   reserved;
            size_t   committed;
        };

        struct alignas(16) LargeHeader
        {
            LargeHeader* next;
            LargeHeader* prev;
            size_t       totalSize;
            size_t       userSize;
        };
    }

    class FixedSizeAllocator
    {
    public:
        void   init(char* region, size_t regionSize, size_t blockSize);
        void   destroy();

        void* alloc();
        void   free(void* p);

        bool   hasLeaks() const { return m_usedBlocks != 0; }

#ifndef NDEBUG
        bool   isValidBlock(const void* p) const;
        void   dumpStat() const;
        void   dumpBlocks() const;
#endif

    private:
        struct PageInfo
        {
            uint32_t freeHead;
            uint32_t fresh;
            uint32_t used;
            uint32_t next;
            uint32_t prev;
            bool     committed;
        };

        static constexpr uint32_t kNil = 0xFFFFFFFFu;

        char* pageAddr(uint32_t i) const { return m_base + static_cast<size_t>(i) * kFsaPageSize; }
        bool   addPage();
        void   decommitPage(uint32_t i);
        void   listPushFront(uint32_t i);
        void   listRemove(uint32_t i);

        char* m_base = nullptr;
        PageInfo* m_pages = nullptr;
        size_t    m_pagesBytes = 0;
        uint32_t  m_pageCount = 0;
        uint32_t  m_highWater = 0;
        uint32_t  m_capacity = 0;
        uint32_t  m_blockSize = 0;
        uint32_t  m_blockShift = 0;
        uint32_t  m_partialHead = kNil;
        uint32_t  m_decommittedHead = kNil;
        uint32_t  m_emptyPages = 0;
        uint32_t  m_committedPages = 0;
        size_t    m_usedBlocks = 0;
    };

    class CoalesceAllocator
    {
    public:
        void   init();
        void   destroy();

        void* alloc(size_t size);
        void   free(void* p);
        bool   owns(const void* p) const;

        bool   hasLeaks() const { return m_usedBlocks != 0; }

#ifndef NDEBUG
        bool   isValidBlock(const void* p) const;
        void   dumpSegments() const;
        void   dumpStat() const;
        void   dumpBlocks() const;
#endif

    private:
        detail::Segment* createSegment();
        bool             grow(size_t need);
        detail::Block* findFit(size_t need) const;
        void* takeBlock(detail::Block* b, size_t need);
        void             insertFree(detail::Block* b);
        void             removeFree(detail::Block* b);
        void             shrinkTail(detail::Segment* seg, detail::Block* b, detail::Block* sentinel);
        void             unlinkSegment(detail::Segment* seg);
        detail::Segment* segmentOf(const void* p) const;

        detail::Segment* m_segments = nullptr;
        detail::Segment* m_primary = nullptr;
        detail::Block* m_freeHead = nullptr;
        size_t           m_usedBlocks = 0;
    };
}

class MemoryAllocator
{
public:
    // Создаёт неинициализированный аллокатор
    MemoryAllocator();

    // Освобождает всю память, запрошенную у ОС
    virtual ~MemoryAllocator();

    MemoryAllocator(const MemoryAllocator&) = delete;
    MemoryAllocator& operator=(const MemoryAllocator&) = delete;

    // Инициализация: запрашивает у ОС страницы и резервирует адреса
    virtual void  init();

    // Деинициализация: возвращает память ОС (в Debug проверяет утечки)
    virtual void  destroy();

    // Выделяет блок, выровненный минимум по 8 байт
    virtual void* alloc(size_t size);

    // Освобождает блок
    virtual void  free(void* p);

#ifndef NDEBUG
    // Статистика (только Debug)
    virtual void  dumpStat() const;

    // Список занятых блоков (только Debug)
    virtual void  dumpBlocks() const;
#endif

private:
    static constexpr size_t kCatCoalesce = mm::kFsaClassCount;
    static constexpr size_t kCatLarge = mm::kFsaClassCount + 1;
    static constexpr size_t kCatCount = mm::kFsaClassCount + 2;

    size_t categoryOf(const void* p) const;
    void* allocLarge(size_t size);
    void   freeLarge(void* p);
    void   releaseAll();
    bool   hasLeaks() const;

    mm::FixedSizeAllocator  m_fsa[mm::kFsaClassCount];
    mm::CoalesceAllocator   m_coalesce;
    char* m_fsaBase = nullptr;
    mm::detail::LargeHeader* m_largeHead = nullptr;
    bool                    m_initialized = false;

#ifndef NDEBUG
    size_t m_statAllocs[kCatCount] = {};
    size_t m_statFrees = 0;
    size_t m_statRequestedBytes = 0;
#endif
};