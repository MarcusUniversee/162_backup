/*
 * mm_alloc.c
 */

#include "mm_alloc.h"

#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>

typedef struct block {
  size_t size;
  bool free;
  
  struct block* next;
  struct block* prev;
} block_t;

static block_t* heap = NULL;

void* mm_malloc(size_t size) {
  if (size == 0) {
    return NULL;
  }
  block_t* last = NULL;
  block_t* blk;
  for (blk = heap; blk != NULL; blk = blk->next) {
    last = blk;
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
        new_blk->prev = blk;
        new_blk->next = blk->next;
        blk->next = new_blk;
        if (new_blk->next != NULL) {
          new_blk->next->prev = new_blk;
        }
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
  block_t* new_blk = sbrk(sizeof(block_t) + size);
  if (new_blk == (void*)-1) {
    return NULL;
  }
  new_blk->size = size;
  new_blk->free = false;
  new_blk->next = NULL;
  new_blk->prev = last;
  if (last != NULL) {
    last->next = new_blk;
  } else {
    //set to head
    heap = new_blk;
  }
  void* ptr = (void*)(new_blk + 1);
  memset(ptr, 0, size);
  return ptr;
}

void* mm_realloc(void* ptr, size_t size) {
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
  if (ptr == NULL) {
    return;
  }
  //subtract one to get the beginning of the metadata
  block_t* blk = (block_t*)ptr - 1;
  blk->free = true;
  //coalescse
  //check next
  block_t* next_blk = blk->next;
  if (next_blk != NULL && next_blk->free) {
    //add next block to current (metadata + space)
    blk->size = blk->size + sizeof(block_t) + next_blk->size;
    //removal
    blk->next = next_blk->next;
    if (next_blk->next != NULL) {
      next_blk->next->prev = blk;
    }
  }
  //check block behind it
  block_t* prev_blk = blk->prev;
  if (prev_blk != NULL && prev_blk->free) {
    //add current block to previous
    prev_blk->size = prev_blk->size + blk->size + sizeof(block_t);
    prev_blk->next = blk->next;
    if (blk->next != NULL) {
      blk->next->prev = prev_blk;
    }
  }
}
