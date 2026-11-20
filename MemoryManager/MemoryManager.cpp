#include "MemoryManager.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifndef NDEBUG
#include <vector>
#endif

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace
{
    namespace os
    {
#if defined(_WIN32)

        // Размер страницы ОС
        size_t pageSize()
        {
            static const size_t s = []() { SYSTEM_INFO si; GetSystemInfo(&si); return static_cast<size_t>(si.dwPageSize); }();
            return s;
        }

        // MEM_RESERVE: зарезервировать адреса без выделения памяти
        void* reserve(size_t size)
        {
            return VirtualAlloc(nullptr, size, MEM_RESERVE, PAGE_NOACCESS);
        }

        // MEM_COMMIT: выделить память в зарезервированном диапазоне
        bool commit(void* addr, size_t size)
        {
            return VirtualAlloc(addr, size, MEM_COMMIT, PAGE_READWRITE) != nullptr;
        }

        // MEM_DECOMMIT: вернуть память ОС, адреса остаются зарезервированными
        void decommit(void* addr, size_t size)
        {
            VirtualFree(addr, size, MEM_DECOMMIT);
        }

        // MEM_RESERVE | MEM_COMMIT: зарезервировать и сразу выделить
        void* allocate(size_t size)
        {
            return VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        }

        // MEM_RELEASE: освободить весь регион
        void release(void* addr, size_t /*size*/)
        {
            VirtualFree(addr, 0, MEM_RELEASE);
        }

#else

        // Размер страницы ОС
        size_t pageSize()
        {
            static const size_t s = static_cast<size_t>(sysconf(_SC_PAGESIZE));
            return s;
        }

        // MEM_RESERVE: зарезервировать адреса без выделения памяти
        void* reserve(size_t size)
        {
            void* p = mmap(nullptr, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
            return p == MAP_FAILED ? nullptr : p;
        }

        // MEM_COMMIT: выделить память в зарезервированном диапазоне
        bool commit(void* addr, size_t size)
        {
            return mprotect(addr, size, PROT_READ | PROT_WRITE) == 0;
        }

        // MEM_DECOMMIT: вернуть память ОС, адреса остаются зарезервированными
        void decommit(void* addr, size_t size)
        {
            mmap(addr, size, PROT_NONE, MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
        }

        // MEM_RESERVE | MEM_COMMIT: зарезервировать и сразу выделить
        void* allocate(size_t size)
        {
            void* p = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            return p == MAP_FAILED ? nullptr : p;
        }

        // MEM_RELEASE: освободить весь регион
        void release(void* addr, size_t size)
        {
            munmap(addr, size);
        }

#endif
    }

    // Округление вверх до кратного a (a - степень двойки)
    inline size_t alignUp(size_t v, size_t a)
    {
        return (v + a - 1) & ~(a - 1);
    }

    // Критическая ошибка: сообщение и аварийное завершение
    [[noreturn]] void fatal(const char* msg)
    {
        std::fprintf(stderr, "MemoryAllocator: fatal error: %s\n", msg);
        std::abort();
    }
}

namespace mm
{
    // Инициализация FSA для одного размера блока
    void FixedSizeAllocator::init(char* region, size_t regionSize, size_t blockSize)
    {
        assert(blockSize >= kFsaMinBlock && blockSize <= kFsaPageSize && (blockSize & (blockSize - 1)) == 0);

        m_base = region;
        m_blockSize = static_cast<uint32_t>(blockSize);
        m_blockShift = 0;
        while ((static_cast<size_t>(1) << m_blockShift) < blockSize)
            ++m_blockShift;

        m_capacity = static_cast<uint32_t>(kFsaPageSize / blockSize);
        m_pageCount = static_cast<uint32_t>(regionSize / kFsaPageSize);

        m_pagesBytes = sizeof(PageInfo) * m_pageCount;
        m_pages = static_cast<PageInfo*>(os::allocate(m_pagesBytes));
        if (!m_pages)
            fatal("cannot allocate FSA page table");

        m_highWater = m_committedPages = m_emptyPages = 0;
        m_partialHead = m_decommittedHead = kNil;
        m_usedBlocks = 0;

        if (!addPage())
            fatal("cannot commit first FSA page");
    }

    // Освобождение таблицы страниц (сам регион освобождает MemoryAllocator)
    void FixedSizeAllocator::destroy()
    {
        if (m_pages)
            os::release(m_pages, m_pagesBytes);
        m_pages = nullptr;
        m_base = nullptr;
        m_pageCount = m_highWater = m_committedPages = m_emptyPages = 0;
        m_partialHead = m_decommittedHead = kNil;
        m_usedBlocks = 0;
    }

    // Добавить страницу в список страниц со свободными блоками
    void FixedSizeAllocator::listPushFront(uint32_t i)
    {
        PageInfo& pg = m_pages[i];
        pg.prev = kNil;
        pg.next = m_partialHead;
        if (m_partialHead != kNil)
            m_pages[m_partialHead].prev = i;
        m_partialHead = i;
    }

    // Убрать страницу из списка страниц со свободными блоками
    void FixedSizeAllocator::listRemove(uint32_t i)
    {
        PageInfo& pg = m_pages[i];
        if (pg.prev != kNil) m_pages[pg.prev].next = pg.next; else m_partialHead = pg.next;
        if (pg.next != kNil) m_pages[pg.next].prev = pg.prev;
        pg.next = pg.prev = kNil;
    }

    // Закоммитить новую (или ранее декоммиченную) страницу
    bool FixedSizeAllocator::addPage()
    {
        const bool fromStack = (m_decommittedHead != kNil);
        const uint32_t i = fromStack ? m_decommittedHead : m_highWater;

        if (!fromStack && i >= m_pageCount)
            return false;

        if (!os::commit(pageAddr(i), kFsaPageSize))
            return false;

        if (fromStack) m_decommittedHead = m_pages[i].next;
        else           ++m_highWater;

        PageInfo& pg = m_pages[i];
        pg.freeHead = kNil;
        pg.fresh = 0;
        pg.used = 0;
        pg.next = pg.prev = kNil;
        pg.committed = true;

        ++m_committedPages;
        ++m_emptyPages;
        listPushFront(i);
        return true;
    }

    // Вернуть пустую страницу ОС (MEM_DECOMMIT)
    void FixedSizeAllocator::decommitPage(uint32_t i)
    {
        listRemove(i);
        os::decommit(pageAddr(i), kFsaPageSize);

        PageInfo& pg = m_pages[i];
        pg.committed = false;
        pg.next = m_decommittedHead;
        m_decommittedHead = i;
        --m_committedPages;
    }

    // Выдать блок: из списка освобождённых или из нетронутой части страницы
    void* FixedSizeAllocator::alloc()
    {
        if (m_partialHead == kNil && !addPage())
            return nullptr;

        const uint32_t pi = m_partialHead;
        PageInfo& pg = m_pages[pi];
        char* page = pageAddr(pi);

        uint32_t bi;
        if (pg.freeHead != kNil)
        {
            bi = pg.freeHead;
            pg.freeHead = *reinterpret_cast<uint32_t*>(page + (static_cast<size_t>(bi) << m_blockShift));
        }
        else
        {
            bi = pg.fresh++;
        }

        if (pg.used == 0)
            --m_emptyPages;
        ++pg.used;
        ++m_usedBlocks;

        if (pg.used == m_capacity)
            listRemove(pi);

        return page + (static_cast<size_t>(bi) << m_blockShift);
    }

    // Вернуть блок в свободный список страницы
    void FixedSizeAllocator::free(void* p)
    {
#ifndef NDEBUG
        if (!isValidBlock(p))
        {
            assert(!"FSA: invalid pointer (not a block start) or double free");
            return;
        }
#endif

        const size_t off = static_cast<size_t>(static_cast<char*>(p) - m_base);
        const uint32_t pi = static_cast<uint32_t>(off / kFsaPageSize);
        const uint32_t bi = static_cast<uint32_t>((off & (kFsaPageSize - 1)) >> m_blockShift);

        PageInfo& pg = m_pages[pi];

        *static_cast<uint32_t*>(p) = pg.freeHead;
        pg.freeHead = bi;

        const bool wasFull = (pg.used == m_capacity);
        --pg.used;
        --m_usedBlocks;

        if (wasFull)
            listPushFront(pi);

        if (pg.used == 0)
        {
            if (m_emptyPages == 0)
                ++m_emptyPages;
            else
                decommitPage(pi);
        }
    }

#ifndef NDEBUG

    // Проверка: указатель - начало занятого блока
    bool FixedSizeAllocator::isValidBlock(const void* p) const
    {
        const uintptr_t off = reinterpret_cast<uintptr_t>(p) - reinterpret_cast<uintptr_t>(m_base);
        if (off >= static_cast<uintptr_t>(m_pageCount) * kFsaPageSize)
            return false;

        const uint32_t pi = static_cast<uint32_t>(off / kFsaPageSize);
        if (pi >= m_highWater || !m_pages[pi].committed)
            return false;

        const uint32_t inPage = static_cast<uint32_t>(off % kFsaPageSize);
        if (inPage & (m_blockSize - 1))
            return false;

        const uint32_t bi = inPage >> m_blockShift;
        const PageInfo& pg = m_pages[pi];
        if (bi >= pg.fresh)
            return false;

        const char* page = pageAddr(pi);
        for (uint32_t i = pg.freeHead; i != kNil; i = *reinterpret_cast<const uint32_t*>(page + (static_cast<size_t>(i) << m_blockShift)))
            if (i == bi)
                return false;
        return true;
    }

    // Статистика по одному FSA
    void FixedSizeAllocator::dumpStat() const
    {
        const size_t totalBlocks = static_cast<size_t>(m_committedPages) * m_capacity;
        std::printf("  %4u B | pages committed: %4u / %4u | blocks used: %8zu | blocks free: %8zu\n",
            m_blockSize, m_committedPages, m_pageCount, m_usedBlocks, totalBlocks - m_usedBlocks);
    }

    // Список занятых блоков FSA
    void FixedSizeAllocator::dumpBlocks() const
    {
        std::vector<unsigned char> isFree;
        for (uint32_t pi = 0; pi < m_highWater; ++pi)
        {
            const PageInfo& pg = m_pages[pi];
            if (!pg.committed || pg.used == 0)
                continue;

            const char* page = pageAddr(pi);
            isFree.assign(m_capacity, 0);
            for (uint32_t i = pg.freeHead; i != kNil; i = *reinterpret_cast<const uint32_t*>(page + (static_cast<size_t>(i) << m_blockShift)))
                isFree[i] = 1;

            for (uint32_t i = 0; i < pg.fresh; ++i)
                if (!isFree[i])
                    std::printf("  %p | %8u bytes | FSA-%u\n",
                        static_cast<const void*>(page + (static_cast<size_t>(i) << m_blockShift)), m_blockSize, m_blockSize);
        }
    }

#endif

    namespace
    {
        using detail::Block;
        using detail::FreeLinks;
        using detail::Segment;

        constexpr size_t kHeader = sizeof(Block);
        constexpr size_t kMinBlock = 32;
        constexpr size_t kUsedFlag = 1;
        constexpr size_t kSegmentReserve = 64 * 1024 * 1024;
        constexpr size_t kInitialCommit = 64 * 1024;
        constexpr size_t kCommitStep = 64 * 1024;
        constexpr size_t kDecommitThreshold = 1024 * 1024;
        const size_t     kSegHeader = alignUp(sizeof(Segment), 16);

        inline size_t sizeOf(const Block* b) { return b->size & ~static_cast<size_t>(15); }
        inline bool   isUsed(const Block* b) { return (b->size & kUsedFlag) != 0; }
        inline bool   isSentinel(const Block* b) { return sizeOf(b) == 0; }

        inline Block* nextOf(Block* b) { return reinterpret_cast<Block*>(reinterpret_cast<char*>(b) + sizeOf(b)); }
        inline const Block* nextOf(const Block* b) { return reinterpret_cast<const Block*>(reinterpret_cast<const char*>(b) + sizeOf(b)); }
        inline Block* prevOf(Block* b) { return reinterpret_cast<Block*>(reinterpret_cast<char*>(b) - b->prevSize); }

        inline FreeLinks* links(Block* b) { return reinterpret_cast<FreeLinks*>(reinterpret_cast<char*>(b) + kHeader); }

        inline Block* firstBlock(const Segment* s) { return reinterpret_cast<Block*>(s->base + kSegHeader); }
        inline Block* sentinelOf(const Segment* s) { return reinterpret_cast<Block*>(s->base + s->committed - kHeader); }

        // Полный размер блока (с заголовком) под запрос в userSize байт
        inline size_t blockSizeFor(size_t userSize)
        {
            const size_t total = alignUp(userSize + kHeader, 16);
            return total < kMinBlock ? kMinBlock : total;
        }
    }

    // Создание первого сегмента
    void CoalesceAllocator::init()
    {
        static_assert(sizeof(Block) == 16, "Block header must be 16 bytes");

        m_segments = m_primary = nullptr;
        m_freeHead = nullptr;
        m_usedBlocks = 0;

        m_primary = createSegment();
        if (!m_primary)
            fatal("cannot reserve address space for the coalesce heap");
    }

    // Освобождение всех сегментов (MEM_RELEASE)
    void CoalesceAllocator::destroy()
    {
        Segment* s = m_segments;
        while (s)
        {
            Segment* next = s->next;
            os::release(s->base, s->reserved);
            s = next;
        }
        m_segments = m_primary = nullptr;
        m_freeHead = nullptr;
        m_usedBlocks = 0;
    }

    // Принадлежит ли указатель одному из сегментов
    bool CoalesceAllocator::owns(const void* p) const
    {
        return segmentOf(p) != nullptr;
    }

    // Найти сегмент по адресу
    Segment* CoalesceAllocator::segmentOf(const void* p) const
    {
        for (Segment* s = m_segments; s; s = s->next)
            if (reinterpret_cast<uintptr_t>(p) - reinterpret_cast<uintptr_t>(s->base) < s->reserved)
                return s;
        return nullptr;
    }

    // Новый сегмент: MEM_RESERVE всего региона, MEM_COMMIT только начала
    Segment* CoalesceAllocator::createSegment()
    {
        char* base = static_cast<char*>(os::reserve(kSegmentReserve));
        if (!base)
            return nullptr;

        if (!os::commit(base, kInitialCommit))
        {
            os::release(base, kSegmentReserve);
            return nullptr;
        }

        Segment* seg = reinterpret_cast<Segment*>(base);
        seg->base = base;
        seg->reserved = kSegmentReserve;
        seg->committed = kInitialCommit;
        seg->next = m_segments;
        m_segments = seg;

        Block* first = firstBlock(seg);
        Block* sent = sentinelOf(seg);
        first->size = static_cast<size_t>(reinterpret_cast<char*>(sent) - reinterpret_cast<char*>(first));
        first->prevSize = 0;
        sent->size = kUsedFlag;
        sent->prevSize = first->size;

        insertFree(first);
        return seg;
    }

    // Убрать сегмент из списка
    void CoalesceAllocator::unlinkSegment(Segment* seg)
    {
        Segment** pp = &m_segments;
        while (*pp != seg)
            pp = &(*pp)->next;
        *pp = seg->next;
    }

    // Добавить блок в начало free-list
    void CoalesceAllocator::insertFree(Block* b)
    {
        FreeLinks* l = links(b);
        l->prev = nullptr;
        l->next = m_freeHead;
        if (m_freeHead)
            links(m_freeHead)->prev = b;
        m_freeHead = b;
    }

    // Убрать блок из free-list
    void CoalesceAllocator::removeFree(Block* b)
    {
        FreeLinks* l = links(b);
        if (l->prev) links(l->prev)->next = l->next; else m_freeHead = l->next;
        if (l->next) links(l->next)->prev = l->prev;
    }

    // Первый подходящий свободный блок (first-fit)
    Block* CoalesceAllocator::findFit(size_t need) const
    {
        for (Block* b = m_freeHead; b; b = links(b)->next)
            if (sizeOf(b) >= need)
                return b;
        return nullptr;
    }

    // Занять блок, остаток отделить как свободный
    void* CoalesceAllocator::takeBlock(Block* b, size_t need)
    {
        removeFree(b);
        const size_t total = sizeOf(b);

        if (total - need >= kMinBlock)
        {
            Block* rest = reinterpret_cast<Block*>(reinterpret_cast<char*>(b) + need);
            rest->size = total - need;
            rest->prevSize = need;
            nextOf(rest)->prevSize = rest->size;
            insertFree(rest);
            b->size = need | kUsedFlag;
        }
        else
        {
            b->size = total | kUsedFlag;
        }

        ++m_usedBlocks;
        return reinterpret_cast<char*>(b) + kHeader;
    }

    // Выделение из free-list с ростом кучи при необходимости
    void* CoalesceAllocator::alloc(size_t size)
    {
        assert(size <= kLargeThreshold);
        const size_t need = blockSizeFor(size);

        for (;;)
        {
            if (Block* b = findFit(need))
                return takeBlock(b, need);

            if (!grow(need))
                return nullptr;
        }
    }

    // Увеличить закоммиченную часть сегмента (MEM_COMMIT) или создать новый сегмент
    bool CoalesceAllocator::grow(size_t need)
    {
        const size_t page = os::pageSize();

        for (Segment* s = m_segments; s; s = s->next)
        {
            Block* sent = sentinelOf(s);
            Block* last = prevOf(sent);
            const size_t have = isUsed(last) ? 0 : sizeOf(last);
            const size_t missing = need - have;

            const size_t room = s->reserved - s->committed;
            const size_t minDelta = alignUp(missing, page);
            if (minDelta > room)
                continue;

            size_t delta = alignUp(missing > kCommitStep ? missing : kCommitStep, page);
            if (delta > room)
                delta = room;

            if (!os::commit(s->base + s->committed, delta))
                continue;

            sent->size = delta;
            s->committed += delta;

            Block* newSent = nextOf(sent);
            newSent->size = kUsedFlag;
            newSent->prevSize = delta;

            if (!isUsed(last))
            {
                last->size += delta;
                newSent->prevSize = last->size;
            }
            else
            {
                insertFree(sent);
            }
            return true;
        }

        return createSegment() != nullptr;
    }

    // Освобождение со слиянием соседних свободных блоков
    void CoalesceAllocator::free(void* p)
    {
        Block* b = reinterpret_cast<Block*>(static_cast<char*>(p) - kHeader);

#ifndef NDEBUG
        if (!isValidBlock(p))
        {
            assert(!"Coalesce: invalid pointer (not a block start) or double free");
            return;
        }
#endif

        --m_usedBlocks;
        size_t size = sizeOf(b);

        Block* next = nextOf(b);
        if (!isUsed(next))
        {
            removeFree(next);
            size += sizeOf(next);
        }

        if (b->prevSize != 0)
        {
            Block* prev = prevOf(b);
            if (!isUsed(prev))
            {
                removeFree(prev);
                size += sizeOf(prev);
                b = prev;
            }
        }

        b->size = size;
        Block* after = nextOf(b);
        after->prevSize = size;

        if (isSentinel(after))
        {
            Segment* seg = segmentOf(b);
            if (b->prevSize == 0 && seg != m_primary)
            {
                unlinkSegment(seg);
                os::release(seg->base, seg->reserved);
                return;
            }
            shrinkTail(seg, b, after);
        }

        insertFree(b);
    }

    // Декоммит большого свободного хвоста сегмента (MEM_DECOMMIT)
    void CoalesceAllocator::shrinkTail(Segment* seg, Block* b, Block* sentinel)
    {
        const size_t page = os::pageSize();

        char* oldEnd = reinterpret_cast<char*>(sentinel) + kHeader;
        char* keepEnd = reinterpret_cast<char*>(alignUp(reinterpret_cast<uintptr_t>(b) + kMinBlock + kHeader, page));

        if (keepEnd >= oldEnd || static_cast<size_t>(oldEnd - keepEnd) < kDecommitThreshold)
            return;

        Block* newSent = reinterpret_cast<Block*>(keepEnd - kHeader);
        b->size = static_cast<size_t>(reinterpret_cast<char*>(newSent) - reinterpret_cast<char*>(b));
        newSent->size = kUsedFlag;
        newSent->prevSize = b->size;

        os::decommit(keepEnd, static_cast<size_t>(oldEnd - keepEnd));
        seg->committed = static_cast<size_t>(keepEnd - seg->base);
    }

#ifndef NDEBUG

    // Проверка: указатель - начало занятого блока
    bool CoalesceAllocator::isValidBlock(const void* p) const
    {
        const Segment* seg = segmentOf(p);
        if (!seg)
            return false;

        const char* target = static_cast<const char*>(p) - kHeader;
        const Block* cur = firstBlock(seg);
        const Block* end = sentinelOf(seg);

        while (cur < end)
        {
            if (reinterpret_cast<const char*>(cur) == target)
                return isUsed(cur);
            cur = nextOf(cur);
        }
        return false;
    }

    // Список регионов, запрошенных у ОС
    void CoalesceAllocator::dumpSegments() const
    {
        for (const Segment* s = m_segments; s; s = s->next)
            std::printf("  Coalesce seg : %p, reserved %zu MB, committed %zu KB\n",
                static_cast<const void*>(s->base), s->reserved >> 20, s->committed >> 10);
    }

    // Статистика по занятым и свободным блокам
    void CoalesceAllocator::dumpStat() const
    {
        size_t usedBlocks = 0, freeBlocks = 0, usedBytes = 0, freeBytes = 0, largestFree = 0;

        for (const Segment* s = m_segments; s; s = s->next)
        {
            for (const Block* b = firstBlock(s); b < sentinelOf(s); b = nextOf(b))
            {
                if (isUsed(b)) { ++usedBlocks; usedBytes += sizeOf(b); }
                else { ++freeBlocks; freeBytes += sizeOf(b); if (sizeOf(b) > largestFree) largestFree = sizeOf(b); }
            }
        }

        std::printf("  blocks used: %zu (%zu bytes incl. headers) | blocks free: %zu (%zu bytes) | largest free block: %zu bytes\n",
            usedBlocks, usedBytes, freeBlocks, freeBytes, largestFree);
    }

    // Список занятых блоков
    void CoalesceAllocator::dumpBlocks() const
    {
        for (const Segment* s = m_segments; s; s = s->next)
            for (const Block* b = firstBlock(s); b < sentinelOf(s); b = nextOf(b))
                if (isUsed(b))
                    std::printf("  %p | %8zu bytes | Coalesce\n",
                        static_cast<const void*>(reinterpret_cast<const char*>(b) + kHeader), sizeOf(b) - kHeader);
    }

#endif
}

// Создаёт неинициализированный аллокатор
MemoryAllocator::MemoryAllocator()
{
}

// Освобождает всю память, запрошенную у ОС
MemoryAllocator::~MemoryAllocator()
{
    assert(!m_initialized && "MemoryAllocator: destroy() was not called before the destructor");
    releaseAll();
}

// Резервирует адреса под FSA и создаёт кучу Coalesce
void MemoryAllocator::init()
{
    assert(!m_initialized && "MemoryAllocator::init: allocator is already initialized");

    m_fsaBase = static_cast<char*>(os::reserve(mm::kFsaClassCount * mm::kFsaRegionSize));
    if (!m_fsaBase)
        fatal("cannot reserve address space for FSA");

    for (size_t i = 0; i < mm::kFsaClassCount; ++i)
        m_fsa[i].init(m_fsaBase + i * mm::kFsaRegionSize, mm::kFsaRegionSize, mm::kFsaMinBlock << i);

    m_coalesce.init();
    m_largeHead = nullptr;
    m_initialized = true;

#ifndef NDEBUG
    std::memset(m_statAllocs, 0, sizeof(m_statAllocs));
    m_statFrees = 0;
    m_statRequestedBytes = 0;
#endif
}

// Есть ли неосвобождённые блоки
bool MemoryAllocator::hasLeaks() const
{
    for (size_t i = 0; i < mm::kFsaClassCount; ++i)
        if (m_fsa[i].hasLeaks())
            return true;
    return m_coalesce.hasLeaks() || m_largeHead != nullptr;
}

// Деинициализация: проверка утечек и возврат памяти ОС
void MemoryAllocator::destroy()
{
    assert(m_initialized && "MemoryAllocator::destroy: allocator is not initialized");

#ifndef NDEBUG
    if (hasLeaks())
    {
        std::printf("\n!!! MEMORY LEAK: the following blocks were not freed:\n");
        dumpBlocks();
    }
    assert(!hasLeaks() && "MemoryAllocator::destroy: memory leak detected");
#endif

    releaseAll();
}

// Возврат всей памяти ОС
void MemoryAllocator::releaseAll()
{
    if (!m_initialized)
        return;

    while (m_largeHead)
    {
        mm::detail::LargeHeader* h = m_largeHead;
        m_largeHead = h->next;
        os::release(h, h->totalSize);
    }

    m_coalesce.destroy();

    for (size_t i = 0; i < mm::kFsaClassCount; ++i)
        m_fsa[i].destroy();
    os::release(m_fsaBase, mm::kFsaClassCount * mm::kFsaRegionSize);
    m_fsaBase = nullptr;

    m_initialized = false;
}

// Определить, какому аллокатору принадлежит указатель
size_t MemoryAllocator::categoryOf(const void* p) const
{
    const uintptr_t off = reinterpret_cast<uintptr_t>(p) - reinterpret_cast<uintptr_t>(m_fsaBase);
    if (off < static_cast<uintptr_t>(mm::kFsaClassCount) * mm::kFsaRegionSize)
        return static_cast<size_t>(off / mm::kFsaRegionSize);
    if (m_coalesce.owns(p))
        return kCatCoalesce;
    return kCatLarge;
}

// Выбор аллокатора по размеру запроса
void* MemoryAllocator::alloc(size_t size)
{
    assert(m_initialized && "MemoryAllocator::alloc: allocator is not initialized");

    if (size == 0)
        size = 1;

    void* p = nullptr;

    if (size <= mm::kFsaMaxBlock)
    {
        size_t cls = 0;
        while ((mm::kFsaMinBlock << cls) < size)
            ++cls;
        p = m_fsa[cls].alloc();
        if (!p)
            p = m_coalesce.alloc(size);
    }
    else if (size <= mm::kLargeThreshold)
    {
        p = m_coalesce.alloc(size);
    }
    else
    {
        p = allocLarge(size);
    }

#ifndef NDEBUG
    if (p)
    {
        ++m_statAllocs[categoryOf(p)];
        m_statRequestedBytes += size;
    }
#endif
    return p;
}

// Освобождение блока
void MemoryAllocator::free(void* p)
{
    assert(m_initialized && "MemoryAllocator::free: allocator is not initialized");

    if (!p)
        return;

#ifndef NDEBUG
    ++m_statFrees;
#endif

    const size_t cat = categoryOf(p);
    if (cat < mm::kFsaClassCount)
        m_fsa[cat].free(p);
    else if (cat == kCatCoalesce)
        m_coalesce.free(p);
    else
        freeLarge(p);
}

// Блок больше 10 МБ - напрямую у ОС
void* MemoryAllocator::allocLarge(size_t size)
{
    using mm::detail::LargeHeader;

    const size_t page = os::pageSize();
    if (size > static_cast<size_t>(-1) - sizeof(LargeHeader) - 2 * page)
        return nullptr;

    const size_t total = alignUp(size + sizeof(LargeHeader), page);
    void* mem = os::allocate(total);
    if (!mem)
        return nullptr;

    LargeHeader* h = static_cast<LargeHeader*>(mem);
    h->totalSize = total;
    h->userSize = size;
    h->prev = nullptr;
    h->next = m_largeHead;
    if (m_largeHead)
        m_largeHead->prev = h;
    m_largeHead = h;

    return static_cast<char*>(mem) + sizeof(LargeHeader);
}

// Вернуть большой блок ОС
void MemoryAllocator::freeLarge(void* p)
{
    using mm::detail::LargeHeader;

    LargeHeader* h = reinterpret_cast<LargeHeader*>(static_cast<char*>(p) - sizeof(LargeHeader));

#ifndef NDEBUG
    bool found = false;
    for (LargeHeader* it = m_largeHead; it; it = it->next)
        if (it == h) { found = true; break; }
    if (!found)
    {
        assert(!"MemoryAllocator::free: pointer was not allocated by this allocator");
        return;
    }
#endif

    if (h->prev) h->prev->next = h->next; else m_largeHead = h->next;
    if (h->next) h->next->prev = h->prev;

    os::release(h, h->totalSize);
}

#ifndef NDEBUG

// Вывод статистики аллокатора
void MemoryAllocator::dumpStat() const
{
    assert(m_initialized && "MemoryAllocator::dumpStat: allocator is not initialized");

    std::printf("\n=================== MemoryAllocator: statistics ===================\n");

    std::printf("\n[Memory requested from the OS]\n");
    std::printf("  FSA region   : %p, reserved %zu MB (%zu x %zu MB), committed on demand by %zu KB pages\n",
        static_cast<const void*>(m_fsaBase), mm::kFsaClassCount * mm::kFsaRegionSize >> 20,
        mm::kFsaClassCount, mm::kFsaRegionSize >> 20, mm::kFsaPageSize >> 10);
    m_coalesce.dumpSegments();

    std::printf("\n[FSA]\n");
    for (size_t i = 0; i < mm::kFsaClassCount; ++i)
        m_fsa[i].dumpStat();

    std::printf("\n[Coalesce]\n");
    m_coalesce.dumpStat();

    std::printf("\n[Blocks allocated directly from the OS (> %zu MB)]\n", mm::kLargeThreshold >> 20);
    size_t largeCount = 0;
    for (const mm::detail::LargeHeader* h = m_largeHead; h; h = h->next)
    {
        ++largeCount;
        std::printf("  %p | user size %zu bytes | OS region %zu bytes\n",
            static_cast<const void*>(reinterpret_cast<const char*>(h) + sizeof(*h)), h->userSize, h->totalSize);
    }
    if (!largeCount)
        std::printf("  (none)\n");

    std::printf("\n[Request histogram: alloc() calls]\n");
    static const char* const labels[kCatCount] = { "16 B", "32 B", "64 B", "128 B", "256 B", "512 B", "Coalesce", "OS >10MB" };
    size_t maxCount = 1;
    for (size_t i = 0; i < kCatCount; ++i)
        if (m_statAllocs[i] > maxCount)
            maxCount = m_statAllocs[i];
    for (size_t i = 0; i < kCatCount; ++i)
    {
        std::printf("  %-9s %8zu  ", labels[i], m_statAllocs[i]);
        const size_t bars = m_statAllocs[i] * 40 / maxCount;
        for (size_t k = 0; k < bars; ++k)
            std::putchar('#');
        std::putchar('\n');
    }
    std::printf("  total requested: %zu bytes, free() calls: %zu\n", m_statRequestedBytes, m_statFrees);
    std::printf("====================================================================\n");
}

// Вывод списка занятых блоков
void MemoryAllocator::dumpBlocks() const
{
    assert(m_initialized && "MemoryAllocator::dumpBlocks: allocator is not initialized");

    std::printf("\n------------------- Occupied blocks -------------------\n");
    std::printf("  address            |     size        | allocator\n");
    for (size_t i = 0; i < mm::kFsaClassCount; ++i)
        m_fsa[i].dumpBlocks();
    m_coalesce.dumpBlocks();
    for (const mm::detail::LargeHeader* h = m_largeHead; h; h = h->next)
        std::printf("  %p | %8zu bytes | OS\n",
            static_cast<const void*>(reinterpret_cast<const char*>(h) + sizeof(*h)), h->userSize);
    std::printf("-------------------------------------------------------\n");
}

#endif