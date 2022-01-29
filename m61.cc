#define M61_DISABLE 1
#include "m61.hh"
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <cinttypes>
#include <cassert>
#include <algorithm>
#include <unordered_map>
using namespace std;

static m61_statistics gstats = {0, 0, 0, 0, 0, 0, 0, 0};
static vector<void*> activeAlloc; // track pointers to active allocations
static unordered_map<string, size_t> heavyTracker; // map code locations to total allocated bytes
static const char magic_key = '*';

bool compHitters(pair<string, size_t> a, pair<string, size_t> b) { // for sorting heavy hitters
    return a.second > b.second;
}

/// m61_malloc(sz, file, line)
///    Return a pointer to `sz` bytes of newly-allocated dynamic memory.
///    The memory is not initialized. If `sz == 0`, then m61_malloc must
///    return a unique, newly-allocated pointer value. The allocation
///    request was at location `file`:`line`.
void* m61_malloc(size_t sz, const char* file, long line) {
    (void) file, (void) line; // avoid uninitialized variable warnings

    // concatenate file and line into one string
    string fileName(file);
    string fileLine = fileName + ":" + to_string(line);

    void* prePtr = base_malloc(sz + sizeof(metAlloc) + 1); // add extra space for metadata and marker

    if (prePtr == nullptr || sizeof(metAlloc) > SIZE_MAX - sz) { // failed allocation
        gstats.nfail++;
        gstats.fail_size += sz;
        return nullptr;
    }
    
    // create a pointer with metadata before the payload and a marker after
    // metadata tracks allocations' size, code location, and status/history
    // marker is overwritten in boundary errors
    metAlloc* meta_ptr = (metAlloc*) prePtr;
    metAlloc metadata = {sz, true, true, file, line};
    *meta_ptr = metadata;
    void* ptr = (void*) ((uintptr_t) meta_ptr + sizeof(metAlloc));
    char* magicPtr = (char*)((uintptr_t) ptr + sz);
    *magicPtr = magic_key;

    if (gstats.ntotal == 0) { // case for if we haven't ever allocated memory yet
        gstats.heap_min = (uintptr_t) ptr;
        gstats.heap_max = (uintptr_t) ptr + sz - 1;
    }
    if ((uintptr_t) ptr < gstats.heap_min) {
        gstats.heap_min = (uintptr_t) ptr;
    }
    if ((uintptr_t) ptr + sz - 1 > gstats.heap_max) {
        gstats.heap_max = (uintptr_t) ptr + sz - 1;
    }

    // update statistics upon successful allocation
    gstats.ntotal++; 
    gstats.nactive++;
    gstats.total_size += sz;
    gstats.active_size += sz;

    // keep track of the active allocations
    activeAlloc.push_back(ptr);

    // account for heavy hitters tracking
    if (heavyTracker.find(fileLine) != heavyTracker.end()) {
        heavyTracker.find(fileLine)->second += sz; 
    } else {
        heavyTracker.insert({fileLine, sz});
    }

    return ptr;
}

/// m61_free(ptr, file, line)
///    Free the memory space pointed to by `ptr`, which must have been
///    returned by a previous call to m61_malloc. If `ptr == NULL`,
///    does nothing. The free was called at location `file`:`line`.
void m61_free(void* ptr, const char* file, long line) {
    (void) file, (void) line; // avoid uninitialized variable warnings

    if (ptr == nullptr) {
        return; // there is nothing to free
    } else if ((uintptr_t) ptr < gstats.heap_min || (uintptr_t) ptr > gstats.heap_max ) {
        fprintf(stderr, "MEMORY BUG: %s:%ld: invalid free of pointer %p, not in heap\n", 
                file, line, ptr);
        abort();
    } else {
        metAlloc* meta_ptr = (metAlloc*) ((uintptr_t) ptr - sizeof(metAlloc));
        char* checkBoundary = (char*) ((uintptr_t) ptr + meta_ptr->sz); // check magic_key
        int index = -1;
        for (int i = 0; i < (int) activeAlloc.size(); i++) {
            if (activeAlloc[i] == ptr) {
                index = i; // save index to be used for removing pointer from activeAlloc
            }
        }
        if (meta_ptr->everAllocated == false) { // has never been allocated in its history
            fprintf(stderr, "MEMORY BUG: %s:%ld: invalid free of pointer %p, not allocated\n", 
                    file, line, ptr);

            for (int i = 0; i < (int) activeAlloc.size(); i++) {
                // save the address range of an allocation
                uintptr_t begin = (uintptr_t)activeAlloc[i] - sizeof(metAlloc);
                metAlloc* bigA = (metAlloc*) begin;
                uintptr_t end = (uintptr_t)activeAlloc[i] + bigA->sz;

                int bytesInside = (int)((uintptr_t) ptr - (uintptr_t)activeAlloc[i]);
                int allocSize = (int) (end - begin - sizeof(metAlloc));

                if ((uintptr_t) ptr >= begin && (uintptr_t) ptr <= end) {
                    fprintf(stderr, "%s:%ld: %p is %i bytes inside a %i byte region allocated here\n", 
                            file, ((metAlloc*) begin)->line, ptr, bytesInside, allocSize);
                }
            }
            abort();
        } else if (meta_ptr->allocated == false) { // inactive now, but has been allocated in its history
            fprintf(stderr, "MEMORY BUG: %s:%ld: invalid free of pointer %p, double free\n", 
                    file, line, ptr);
            abort();
        } else if (*checkBoundary != '*'){ // marker has been overwritten
            fprintf(stderr, "MEMORY BUG: %s:%ld: detected wild write during free of pointer %p\n", 
                    file, line, ptr);
            abort();
        } else {
            for (int i = 0; i < (int) activeAlloc.size(); i++) {
                uintptr_t begin = (uintptr_t)activeAlloc[i] - sizeof(metAlloc);
                metAlloc* bigA = (metAlloc*) begin;
                uintptr_t end = (uintptr_t)activeAlloc[i] + bigA->sz;
            
                // checks for wild frees where the middle of a memory block is trying to be freed
                // but there happens to be the beginning of another block copied to that location
                if ((uintptr_t) ptr > begin && (uintptr_t) ptr < end && (uintptr_t) ptr != begin + 
                        sizeof(metAlloc)) {
                    fprintf(stderr, "MEMORY BUG: %s:%ld: invalid free of pointer %p, not allocated\n", 
                            file, line, ptr);
                    abort();    
                }
            }

            gstats.nactive--;
            meta_ptr->allocated = false;
            gstats.active_size -= meta_ptr->sz;
            activeAlloc.erase(activeAlloc.begin() + index);
            base_free(meta_ptr);
        }
    }
}

/// m61_calloc(nmemb, sz, file, line)
///    Return a pointer to newly-allocated dynamic memory big enough to
///    hold an array of `nmemb` elements of `sz` bytes each. If `sz == 0`,
///    then must return a unique, newly-allocated pointer value. Returned
///    memory should be initialized to zero. The allocation request was at
///    location `file`:`line`.
void* m61_calloc(size_t nmemb, size_t sz, const char* file, long line) {
    if (sz > SIZE_MAX / nmemb) { 
        gstats.nfail++;
        gstats.fail_size += nmemb * sz;
        return nullptr;
    }

    void* ptr = m61_malloc(nmemb * sz, file, line);

    if (ptr) {
        memset(ptr, 0, nmemb * sz);
    } else {
        return nullptr;
    }

    return ptr;
}

/// m61_get_statistics(stats)
///    Store the current memory statistics in `*stats`.
void m61_get_statistics(m61_statistics* stats) {
    memset(stats, 0, sizeof(m61_statistics));
    *stats = gstats;
}

/// m61_print_statistics()
///    Print the current memory statistics.
void m61_print_statistics() {
    m61_statistics stats;
    m61_get_statistics(&stats);

    printf("alloc count: active %10llu   total %10llu   fail %10llu\n",
           stats.nactive, stats.ntotal, stats.nfail);
    printf("alloc size:  active %10llu   total %10llu   fail %10llu\n",
           stats.active_size, stats.total_size, stats.fail_size);
}

/// m61_print_leak_report()
///    Print a report of all currently-active allocated blocks of dynamic
///    memory.
void m61_print_leak_report() {
    for (int i = 0; i < (int) activeAlloc.size(); i++) {
        metAlloc* bigA = (metAlloc*) ((uintptr_t) activeAlloc[i] - sizeof(metAlloc));
        printf("LEAK CHECK: %s:%ld: allocated object %p with size %i\n", 
                bigA->file, bigA->line, activeAlloc[i], (int) bigA->sz);
    }
}

/// m61_print_heavy_hitter_report()
///    Print a report of heavily-used allocation locations.
void m61_print_heavy_hitter_report() {
    // transfer code location hash table to a vector and sort it
    vector<pair<string, size_t>> fileLines(heavyTracker.begin(), heavyTracker.end());
    sort(fileLines.begin(), fileLines.end(), compHitters);

    for (int i = 0; i < (int) fileLines.size(); i++) {
        double percent = 100 * (double) fileLines[i].second / gstats.total_size;
        if (percent > 5) { // define heavy hitter as >5% of the total allocated bytes
            printf("HEAVY HITTER: %s: %i bytes (~%f%%)\n", 
                    fileLines[i].first.c_str(), (int) fileLines[i].second, percent);
        }
    }
}

/// m61_realloc(ptr, sz, file, line)
///    Reallocate the dynamic memory pointed to by `ptr` to hold at least
///    `sz` bytes, returning a pointer to the new block. If `ptr` is
///    `nullptr`, behaves like `m61_malloc(sz, file, line)`. If `sz` is 0,
///    behaves like `m61_free(ptr, file, line)`. The allocation request
///    was at location `file`:`line`.
void* m61_realloc(void* ptr, size_t sz, const char* file, long line) {
    if (sz == 0) { // behave just like m61_free
        m61_free(ptr, file, line);
        return nullptr;
    }

    void* newPtr = m61_malloc(sz, file, line);
    if (ptr == nullptr) { // behave just like m61_malloc
    } else {
        metAlloc* meta_ptr = (metAlloc*) (uintptr_t) ptr - sizeof(metAlloc);
        int ptrSize = (int) meta_ptr->sz;
        int byteSize = min<int>((int)sz, ptrSize); // accounts for expansions and contractions

        // copy over the memory from our previous location to our newly allocated memory
        memcpy(newPtr, ptr, byteSize);
        m61_free(ptr, file, line);
    }
    return newPtr;
}