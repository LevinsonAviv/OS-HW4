#include <unistd.h>
#include <string.h>

#define MAX_MALLOC 100000000

struct MallocMetadata {
    size_t size;
    bool is_free;
    MallocMetadata* next;
    MallocMetadata* prev;
};

// API functions
void* smalloc(size_t);
void* scalloc(size_t, size_t);
void sfree(void*);
void* srealloc(void*, size_t);

// statistics functions
size_t _num_free_bytes();
size_t _num_free_blocks();
size_t _num_allocated_blocks();
size_t _num_allocated_bytes();
size_t _num_meta_data_bytes();
size_t _size_meta_data();

// heap allocator helper functions
MallocMetadata* _get_free_block(size_t);
MallocMetadata* _allocate_new_block(size_t);

// global variables
MallocMetadata *blockListHead = NULL;
MallocMetadata *blockListTail = NULL;

// =======================================
// API functions
// =======================================

void* smalloc(size_t size) {
    MallocMetadata *newBlock;
    if(size == 0 || size > MAX_MALLOC || (newBlock = _get_free_block(size)) == NULL){
        return NULL;
    }
    return (void*)((char*)newBlock + _size_meta_data()); 
}

void* scalloc(size_t num, size_t size) {
    void* newMem;
    size_t total_size = num * size;
    if((newMem = smalloc(total_size)) == NULL){
        return NULL;
    }
    memset(newMem, 0, total_size); // make sure that we should zero a free block if exists
    return newMem;
}

void sfree(void* p) {
    if(p == NULL){
        return;
    }
    MallocMetadata *blockMetadata = (MallocMetadata*)((char*)p - _size_meta_data());
    if(blockMetadata->is_free){
        return;
    }
    blockMetadata->is_free = true;
}

void* srealloc(void* oldp, size_t size) {
    if(oldp == NULL){
        return smalloc(size);
    }
    MallocMetadata *blockMetadata = (MallocMetadata*)((char*)oldp - _size_meta_data());
    if(size <= blockMetadata->size){
        return oldp;
    }
    void* newp;
    if((newp = smalloc(size)) == NULL){
        return NULL;
    }
    memmove(newp, oldp, blockMetadata->size);
    sfree(oldp);
    return newp;
}

// =======================================
// statistics functions
// =======================================

size_t _num_free_blocks() {
    size_t num = 0;
    MallocMetadata *currentBlock = blockListHead;
    while(currentBlock != NULL){
        if(currentBlock->is_free){
            num ++;
        }
        currentBlock = currentBlock->next;
    }
    return num;
}

size_t _num_free_bytes() {
    size_t num = 0;
    MallocMetadata *currentBlock = blockListHead;
    while(currentBlock != NULL){
        if(currentBlock->is_free){
            num += currentBlock->size;
        }
        currentBlock = currentBlock->next;
    }
    return num;
}

size_t _num_allocated_blocks() {
    size_t num = 0;
    MallocMetadata *currentBlock = blockListHead;
    while(currentBlock != NULL){
        num ++;
        currentBlock = currentBlock->next;
    }
    return num;
}

size_t _num_allocated_bytes() {
    size_t num = 0;
    MallocMetadata *currentBlock = blockListHead;
    while(currentBlock != NULL){
        num += currentBlock->size;
        currentBlock = currentBlock->next;
    }
    return num;
}

size_t _num_meta_data_bytes() {
    return _num_allocated_blocks() * _size_meta_data();
}

size_t _size_meta_data() {
    static size_t size = sizeof(MallocMetadata);
    return size;
}

// =======================================
// heap allocator helper functions
// =======================================

MallocMetadata* _get_free_block(size_t size) {
    MallocMetadata *currentBlock = blockListHead;
    while(currentBlock != NULL){
        if(currentBlock->is_free && currentBlock->size >= size){
            currentBlock->is_free = false;
            return currentBlock;
        }
        currentBlock = currentBlock->next;
    }
    return _allocate_new_block(size);
}

MallocMetadata* _allocate_new_block(size_t size) {
    void *newPtr;
    if((newPtr = sbrk(size + _size_meta_data())) == (void*)(-1)){
        return NULL;
    }
    MallocMetadata *newMetadata = (MallocMetadata*)newPtr;
    newMetadata->is_free = false;
    newMetadata->size = size;
    newMetadata->next = NULL;
    newMetadata->prev = blockListTail;

    if(blockListHead == NULL){
        blockListHead = newMetadata;
    }
    if(blockListTail != NULL){
        blockListTail->next = newMetadata;
    }
    blockListTail = newMetadata;

    return newMetadata;
}
    