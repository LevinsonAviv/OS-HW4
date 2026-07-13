#include <unistd.h>
#include <string.h>
#include <sys/mman.h>

#define MAX_MALLOC 100000000
#define HEAP_SIZE (32 * 128 * 1024)
#define MIN_BLOCK_SIZE 128
#define MAX_BLOCK_SIZE (128 * 1024)
#define MAX_ORDER 10

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
bool _init_blocks();
int _calculate_order(size_t);
MallocMetadata* _find_buddy(MallocMetadata*);
MallocMetadata* _pop_first_free_block(int);
MallocMetadata* _pop_first_free_sub_block(int, int);
void _insert_free_block(MallocMetadata*);
void _insert_free_block_merge(MallocMetadata *);
void _remove_free_block(MallocMetadata*);
bool _can_merge_for_srealloc(MallocMetadata*, size_t);
MallocMetadata* _merge_for_srealloc(MallocMetadata*, size_t);
MallocMetadata* _get_free_block(size_t);

// mmap allocator helper functions
void* _add_mmap(size_t);
void _remove_mmap(MallocMetadata*);

// global variables
MallocMetadata* freeBlocksByOrder[MAX_ORDER + 1] = {NULL};
MallocMetadata *firstHeapBlock = NULL;
MallocMetadata *mmapListHead = NULL;

// =======================================
// API functions
// =======================================

void* smalloc(size_t size) {
    if(firstHeapBlock == NULL && _init_blocks() == false){
        return NULL;
    }

    if(size == 0 || size > MAX_MALLOC) {
        return NULL;
    }

    if(size + _size_meta_data() > MAX_BLOCK_SIZE){
        return _add_mmap(size);
    }

    MallocMetadata *newBlock;
    if((newBlock = _get_free_block(size)) == NULL){
        return NULL;
    }
    newBlock->is_free = false;
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
    if(blockMetadata->size > MAX_BLOCK_SIZE){
        _remove_mmap(blockMetadata);
    }
    else{
        blockMetadata->is_free = true;
        _insert_free_block_merge(blockMetadata);
    }
}

void* srealloc(void* oldp, size_t size) {
    if(size == 0 || size > MAX_MALLOC) {
        return NULL;
    }
    if(oldp == NULL){
        return smalloc(size);
    }

    MallocMetadata *blockMetadata = (MallocMetadata*)((char*)oldp - _size_meta_data());
    size_t required_size = size + _size_meta_data();

    if(blockMetadata->size > MAX_BLOCK_SIZE){
        if(required_size == blockMetadata->size){
            return oldp;
        }

        void* newp = smalloc(size);
        if(newp == NULL){
            return NULL;
        }

        memmove(newp, oldp, blockMetadata->size - _size_meta_data());
        sfree(oldp);
        return newp;
    }

    if(required_size <= blockMetadata->size){
        return oldp;
    }

    if(_can_merge_for_srealloc(blockMetadata, required_size)){
        blockMetadata = _merge_for_srealloc(blockMetadata, required_size);
        return (void*)((char*)blockMetadata + _size_meta_data());
    }

    void* newp;
    if((newp = smalloc(size)) == NULL){
        return NULL;
    }
    memmove(newp, oldp, blockMetadata->size - _size_meta_data());
    sfree(oldp);
    return newp;
}

// =======================================
// statistics functions
// =======================================

size_t _num_free_blocks() {
    if(firstHeapBlock == NULL){
        return 0;
    }
    size_t num = 0;
    for(int i = 0; i <= MAX_ORDER; i++){
        MallocMetadata *currentBlock = freeBlocksByOrder[i];
        while(currentBlock != NULL){
            num++;
            currentBlock = currentBlock->next;
        }
    }
    return num;
}

size_t _num_free_bytes() {
    if(firstHeapBlock == NULL){
        return 0;
    }
    size_t num = 0;
    for(int i = 0; i <= MAX_ORDER; i++){
        MallocMetadata *currentBlock = freeBlocksByOrder[i];
        while(currentBlock != NULL){
            num += (currentBlock->size - _size_meta_data());
            currentBlock = currentBlock->next;
        }
    }
    return num;
}

size_t _num_allocated_blocks() {
    if(firstHeapBlock == NULL){
        return 0;
    }
    size_t num = 0;

    MallocMetadata *currentBlock = firstHeapBlock;
    void* heapEnd = (void*)((char*)firstHeapBlock + HEAP_SIZE);
    while((void*)currentBlock < heapEnd){
        num++;
        currentBlock = (MallocMetadata*)((char*)currentBlock + currentBlock->size);
    }

    currentBlock = mmapListHead;
    while(currentBlock != NULL){
        num++;
        currentBlock = currentBlock->next;
    }

    return num;
}

size_t _num_allocated_bytes() {
    if(firstHeapBlock == NULL){
        return 0;
    }
    size_t num = 0;
    
    MallocMetadata *currentBlock = firstHeapBlock;
    void* heapEnd = (void*)((char*)firstHeapBlock + HEAP_SIZE);
    while((void*)currentBlock < heapEnd){
        num += (currentBlock->size - _size_meta_data());
        currentBlock = (MallocMetadata*)((char*)currentBlock + currentBlock->size);
    }

    currentBlock = mmapListHead;
    while(currentBlock != NULL){
        num += (currentBlock->size - _size_meta_data());
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

bool _init_blocks() {
    void* current_bp;
    if((current_bp = sbrk(0)) == (void*)(-1)){
        return false;
    }

    intptr_t current_addr = (intptr_t)current_bp;
    intptr_t align_off = (HEAP_SIZE - current_addr % HEAP_SIZE) % HEAP_SIZE;

    if(sbrk(align_off + HEAP_SIZE) == (void*)(-1)){
        return false;
    }

    firstHeapBlock = (MallocMetadata*)((char*)current_bp + align_off);
    for(int i = 0; i < 32; i++){
        MallocMetadata *currentBlock = (MallocMetadata*)((char*)firstHeapBlock + (i * MAX_BLOCK_SIZE));
        currentBlock->size = MAX_BLOCK_SIZE;
        currentBlock->is_free = true;
        currentBlock->next = NULL;
        currentBlock->prev = NULL;
        _insert_free_block(currentBlock);
    }

    return true;
}

int _calculate_order(size_t total_size) {
    if(total_size > MAX_BLOCK_SIZE){
        return -1;
    }
    int order = 0;
    size_t block_size = MIN_BLOCK_SIZE;
    while(block_size < total_size && order < MAX_ORDER){
        order++;
        block_size <<= 1;
    }
    return order; // check if size is too big
}

MallocMetadata* _find_buddy(MallocMetadata* blockMetadata) {
    if(blockMetadata->size == MAX_BLOCK_SIZE){
        return NULL;
    }
    return (MallocMetadata*)((intptr_t)blockMetadata ^ blockMetadata->size);
}

MallocMetadata* _pop_first_free_block(int order) {
    if(order < 0 || order > MAX_ORDER){
        return NULL;
    }
    MallocMetadata *blockMetadata = freeBlocksByOrder[order];
    if(blockMetadata != NULL){
        freeBlocksByOrder[order] = blockMetadata->next;
        if(freeBlocksByOrder[order] != NULL){
            freeBlocksByOrder[order]->prev = NULL;
        }
        blockMetadata->next = NULL;
        blockMetadata->prev = NULL;
    }
    return blockMetadata;
}

MallocMetadata* _pop_first_free_sub_block(int block_order, int sub_block_order) {
    MallocMetadata *blockMetadata = _pop_first_free_block(block_order);

    while(sub_block_order < block_order){
        block_order--;

        blockMetadata->size /= 2;
        MallocMetadata *buddyBlock = (MallocMetadata*)((char*)blockMetadata + blockMetadata->size);

        buddyBlock->size = blockMetadata->size;
        buddyBlock->is_free = true;
        buddyBlock->next = NULL;
        buddyBlock->prev = NULL;

        _insert_free_block(buddyBlock);
    }

    return blockMetadata;
}

void _insert_free_block(MallocMetadata *blockMetadata) {
    int order = _calculate_order(blockMetadata->size);
    if(order == -1){
        return;
    }

    MallocMetadata *currentBlock = freeBlocksByOrder[order];
    MallocMetadata *prevBlock = NULL;

    while(currentBlock != NULL && blockMetadata > currentBlock){
        prevBlock = currentBlock;
        currentBlock = currentBlock->next;
    }
    if(blockMetadata == currentBlock){ // block is already free
        return;
    }

    blockMetadata->next = currentBlock;
    blockMetadata->prev = prevBlock;
    if(currentBlock != NULL){
        currentBlock->prev = blockMetadata;
    }
    if(prevBlock != NULL){
        prevBlock->next = blockMetadata;
    }
    else{
        freeBlocksByOrder[order] = blockMetadata;
    }
}

void _insert_free_block_merge(MallocMetadata *blockMetadata) {
    int block_order = _calculate_order(blockMetadata->size);
    MallocMetadata *buddyBlock = _find_buddy(blockMetadata);

    while(block_order < MAX_ORDER && buddyBlock != NULL && blockMetadata->size == buddyBlock->size && buddyBlock->is_free == true){
        _remove_free_block(buddyBlock);

        blockMetadata = (buddyBlock < blockMetadata) ? buddyBlock : blockMetadata;

        blockMetadata->size *= 2;
        block_order++;
        buddyBlock = _find_buddy(blockMetadata);
    }

    _insert_free_block(blockMetadata);
}

void _remove_free_block(MallocMetadata *blockMetadata) {
    if(blockMetadata->next != NULL){
        blockMetadata->next->prev = blockMetadata->prev;
    }
    if(blockMetadata->prev != NULL){
        blockMetadata->prev->next = blockMetadata->next;
    }
    else{
        freeBlocksByOrder[_calculate_order(blockMetadata->size)] = blockMetadata->next;
    }
}

bool _can_merge_for_srealloc(MallocMetadata* blockMetadata, size_t required_size) {
    size_t current_size = blockMetadata->size;
    MallocMetadata* currentBlock = blockMetadata;
    
    while(current_size < required_size && current_size < MAX_BLOCK_SIZE){
        MallocMetadata *buddyBlock = (MallocMetadata*)((intptr_t)currentBlock ^ current_size);

        if(buddyBlock != NULL && buddyBlock->size == current_size && buddyBlock->is_free == true){
            current_size *= 2;
            currentBlock = (buddyBlock < currentBlock) ? buddyBlock : currentBlock;
        }
        else{
            break;
        }
    }

    return current_size >= required_size;
}

MallocMetadata* _merge_for_srealloc(MallocMetadata* blockMetadata, size_t required_size) {
    size_t old_size = blockMetadata->size;
    MallocMetadata* currentBlock = blockMetadata;
    MallocMetadata *buddyBlock = _find_buddy(blockMetadata);

    while(currentBlock->size < required_size && currentBlock->size < MAX_BLOCK_SIZE && buddyBlock != NULL && buddyBlock->size == currentBlock->size && buddyBlock->is_free == true){
        _remove_free_block(buddyBlock);
        currentBlock = (buddyBlock < currentBlock) ? buddyBlock : currentBlock;

        currentBlock->size *= 2;
        buddyBlock = _find_buddy(currentBlock);
    }

    if(blockMetadata != currentBlock){
        memmove((char*)currentBlock + _size_meta_data(), (char*)blockMetadata + _size_meta_data(), old_size - _size_meta_data());
        currentBlock->is_free = false;
        blockMetadata = currentBlock;
    }

    return blockMetadata;
}

MallocMetadata* _get_free_block(size_t size) {
    int sub_block_order = _calculate_order(size + _size_meta_data());
    if(sub_block_order == -1){
        return NULL;
    }

    int block_order = sub_block_order;

    while(block_order <= MAX_ORDER && freeBlocksByOrder[block_order] == NULL){
        block_order++;
    }

    if(block_order > MAX_ORDER){
        return NULL;
    }

    MallocMetadata *blockMetadata = _pop_first_free_sub_block(block_order, sub_block_order);
    // check what to do if order is MAX and there is no space, we assume there is a suitable block
    return blockMetadata;
}

// =======================================
// mmap allocator helper functions
// =======================================

void* _add_mmap(size_t size) {
    size_t total_size = size + _size_meta_data();

    void* newMem;
    if((newMem = mmap(NULL, total_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0)) == MAP_FAILED){
        return NULL;
    }

    MallocMetadata *blockMetadata = (MallocMetadata*)newMem;
    blockMetadata->size = total_size;
    blockMetadata->is_free = false;

    blockMetadata->next = mmapListHead;
    blockMetadata->prev = NULL;
    if(mmapListHead != NULL){
        mmapListHead->prev = blockMetadata;
    }
    mmapListHead = blockMetadata;

    return (void*)((char*)blockMetadata + _size_meta_data());
}

void _remove_mmap(MallocMetadata* blockMetadata) {
    if(blockMetadata->next != NULL){
        blockMetadata->next->prev = blockMetadata->prev;
    }
    if(blockMetadata->prev != NULL){
        blockMetadata->prev->next = blockMetadata->next;
    }
    else{
        mmapListHead = blockMetadata->next;
    }
    munmap((void*)blockMetadata, blockMetadata->size);
}