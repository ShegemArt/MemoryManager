#include "MemoryManager.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>

// Пример из задания
static void exampleFromTask()
{
    std::printf("### Example from the task\n");

    MemoryAllocator allocator;
    allocator.init();

    int* pi = (int*)allocator.alloc(sizeof(int));
    double* pd = (double*)allocator.alloc(sizeof(double));
    int* pa = (int*)allocator.alloc(10 * sizeof(int));

    *pi = 42;
    *pd = 3.14;
    for (int i = 0; i < 10; ++i)
        pa[i] = i;

#ifndef NDEBUG
    allocator.dumpStat();
    allocator.dumpBlocks();
#endif

    allocator.free(pa);
    allocator.free(pd);
    allocator.free(pi);
    allocator.destroy();
}

// FSA, Coalesce и блок напрямую от ОС
static void exampleAllKinds()
{
    std::printf("\n### FSA + Coalesce + OS\n");

    MemoryAllocator allocator;
    allocator.init();

    void* small = allocator.alloc(100);
    void* medium = allocator.alloc(4096);
    void* big = allocator.alloc(3u * 1024 * 1024);
    void* huge = allocator.alloc(12u * 1024 * 1024);

    std::memset(huge, 0xAB, 12u * 1024 * 1024);

#ifndef NDEBUG
    allocator.dumpStat();
    allocator.dumpBlocks();
#endif

    allocator.free(huge);
    allocator.free(big);
    allocator.free(medium);
    allocator.free(small);
    allocator.destroy();
}

struct Record
{
    unsigned char* ptr;
    size_t         size;
    unsigned char  pattern;
};

// Заполнить блок паттерном
static void fillBlock(const Record& r) { std::memset(r.ptr, r.pattern, r.size); }

// Проверить, что содержимое блока не испорчено
static bool checkBlock(const Record& r)
{
    for (size_t i = 0; i < r.size; ++i)
        if (r.ptr[i] != r.pattern)
            return false;
    return true;
}

// Случайный размер запроса
static size_t randomSize(std::mt19937& rng)
{
    const unsigned k = rng() % 100;
    if (k < 70) return 1 + rng() % 512;
    if (k < 95) return 513 + rng() % (64 * 1024);
    if (k < 99) return 1 + rng() % (2 * 1024 * 1024);
    return 10u * 1024 * 1024 + 1 + rng() % (2 * 1024 * 1024);
}

// Случайные alloc/free с проверкой содержимого блоков
static bool stressTest(unsigned iterations)
{
    std::printf("\n### Stress test (%u iterations)\n", iterations);

    MemoryAllocator allocator;
    allocator.init();

    std::mt19937 rng(12345);
    std::vector<Record> live;
    size_t liveBytes = 0;
    const size_t kMaxLiveBytes = 128u * 1024 * 1024;
    bool ok = true;

    for (unsigned it = 0; it < iterations && ok; ++it)
    {
        const bool doAlloc = live.empty() || (liveBytes < kMaxLiveBytes && (rng() % 100) < 55);
        if (doAlloc)
        {
            Record r;
            r.size = randomSize(rng);
            r.ptr = (unsigned char*)allocator.alloc(r.size);
            r.pattern = (unsigned char)(rng() & 0xFF);
            if (!r.ptr || ((uintptr_t)r.ptr & 7) != 0)
            {
                std::printf("FAIL: alloc(%zu) returned %p\n", r.size, (void*)r.ptr);
                ok = false;
                break;
            }
            fillBlock(r);
            liveBytes += r.size;
            live.push_back(r);
        }
        else
        {
            const size_t idx = rng() % live.size();
            if (!checkBlock(live[idx]))
            {
                std::printf("FAIL: block %p (size %zu) was corrupted\n", (void*)live[idx].ptr, live[idx].size);
                ok = false;
                break;
            }
            allocator.free(live[idx].ptr);
            liveBytes -= live[idx].size;
            live[idx] = live.back();
            live.pop_back();
        }
    }

    for (const Record& r : live)
    {
        if (ok && !checkBlock(r))
        {
            std::printf("FAIL: block %p (size %zu) was corrupted\n", (void*)r.ptr, r.size);
            ok = false;
        }
        allocator.free(r.ptr);
    }

    allocator.destroy();
    std::printf(ok ? "Stress test passed\n" : "Stress test FAILED\n");
    return ok;
}

int main()
{
    exampleFromTask();
    exampleAllKinds();
    return stressTest(30000) ? 0 : 1;
}