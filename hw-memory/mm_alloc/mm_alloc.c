/*
 * mm_alloc.c
 */

#include "mm_alloc.h"

#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include "../pintos/src/lib/kernel/list.h"

typedef struct block {
  size_t size;
  bool free;
  
  struct list_elem elem;
} block_t;

static struct list heap;
static bool heap_init = false;

void* mm_malloc(size_t size) {
  if (!heap_init) {
    list_init(&heap);
    heap_init = true;
  }
  if (size == 0) {
    return NULL;
  }
  struct list_elem* e;
  for (e = list_begin(&heap); e != list_end(&heap); e = list_next(e)) {
    block_t* blk = list_entry(e, block_t, elem);
    if (blk->free) {
      //check for free block
      if (blk->size > size + sizeof(block_t)) {
        //split block into two
        //new size not have the size + new metadata
        size_t new_size = blk->size - sizeof(block_t) - size;
        //change the old block size
        blk->size = size;
        blk->free = false;
        void* ptr = (void*)(blk + 1);
        memset(ptr, 0, size);

        //using the remaining space, allocate the other block
        block_t* new_blk = (block_t*) ((char*)(blk + 1) + blk->size);
        new_blk->free = true;
        new_blk->size = new_size;
        list_insert(list_next(&blk->elem), &new_blk->elem);
        return ptr;
      } else if (blk->size >= size) {
        //allocate entire block, but do not allocate a new one
        blk->free = false;
        void* ptr = (void*)(blk + 1);
        memset(ptr, 0, size);
        return ptr;
      }
    }
  }
  //we need to allocate more free space
  block_t* blk = sbrk(sizeof(block_t) + size);
  if (blk == (void*)-1) {
    return NULL;
  }
  blk->size = size;
  blk->free = false;
  list_push_back(&heap, &blk->elem);
  void* ptr = (void*)(blk + 1);
  memset(ptr, 0, size);
  return ptr;
}

void* mm_realloc(void* ptr, size_t size) {
  if (!heap_init) {
    list_init(&heap);
    heap_init = true;
  }
  //edge case
  if (size == 0) {
    mm_free(ptr);
    return NULL;
  }
  //first mm_malloc a block
  void* new_ptr = mm_malloc(size);
  if (new_ptr == NULL) {
    return NULL;
  }
  if (ptr == NULL) {
    return new_ptr;
  }
  //get the metadata for the old block
  block_t* blk = (block_t*)ptr - 1;

  //copy over whatever size is smaller
  size_t copy_size = blk->size < size ? blk->size : size;
  //then memcpy
  memcpy(new_ptr, ptr, copy_size);
  mm_free(ptr);

  return new_ptr;
}

void mm_free(void* ptr) {
  if (!heap_init) {
    list_init(&heap);
    heap_init = true;
  }
  if (ptr == NULL) {
    return;
  }
  //subtract one to get the beginning of the metadata
  block_t* blk = (block_t*)ptr - 1;
  blk->free = true;
  //coalescse
  struct list_elem* e;
  //check the block in front of it
  e = list_next(&blk->elem);
  block_t* next_blk = list_entry(e, block_t, elem);
  if (next_blk->free) {
    //add next block to current (metadata + space)
    blk->size = blk->size + sizeof(block_t) + next_blk->size;
    list_remove(e);
  }
  //check block behind it
  e = list_prev(&blk->elem);
  block_t* prev_blk = list_entry(e, block_t, elem);
  if (prev_blk->free) {
    //add current block to previous
    prev_blk->size = prev_blk->size + blk->size + sizeof(block_t);
    list_remove(&blk->elem);
  }
}
