#include <unistd.h>

#define MAX_MALLOC 100000000

void* smalloc(size_t size) {
    void *resultPtr;
    if(size == 0 || size > MAX_MALLOC || (resultPtr = sbrk(size)) == (void*)(-1)){
        return NULL;
    }
    return resultPtr; 
}