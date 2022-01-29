#include "m61.hh"
#include <cstdio>
#include <cstring>
#include <cstdlib>
using namespace std;
// Reallocate memory to hold more bytes.

size_t getsize(void * p) {
    size_t* in = (size_t*) p;
    if (in) { --in; return *in; }
    return -1;
}

int main() {
    char* aa = (char*) malloc(50);
    char* bb;
    uintptr_t add = (uintptr_t) aa - sizeof(metAlloc);
    metAlloc* x = (metAlloc*) add;
    printf("aa size is %d\n", (int) x->sz);
    strcpy(aa,"my cookie");

    bb = (char*) realloc(aa, 51);
    uintptr_t add2 = (uintptr_t) bb - sizeof(metAlloc);
    metAlloc* y = (metAlloc*) add2;
    printf("bb size is %d\n", (int) y->sz);
}

//! aa size is 50
//! bb size is 51