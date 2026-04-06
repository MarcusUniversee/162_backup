#include "userprog/syscall.h"
#include <stdio.h>
#include <string.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "filesys/filesys.h"
#include "filesys/file.h"
#include "threads/palloc.h"

static void syscall_handler(struct intr_frame*);

void syscall_init(void) { intr_register_int(0x30, 3, INTR_ON, syscall_handler, "syscall"); }

void syscall_exit(int status) {
  printf("%s: exit(%d)\n", thread_current()->name, status);
  thread_exit();
}

/*
 * This does not check that the buffer consists of only mapped pages; it merely
 * checks the buffer exists entirely below PHYS_BASE.
 */
static void validate_buffer_in_user_region(const void* buffer, size_t length) {
  uintptr_t delta = PHYS_BASE - buffer;
  if (!is_user_vaddr(buffer) || length > delta)
    syscall_exit(-1);
}

/*
 * This does not check that the string consists of only mapped pages; it merely
 * checks the string exists entirely below PHYS_BASE.
 */
static void validate_string_in_user_region(const char* string) {
  uintptr_t delta = PHYS_BASE - (const void*)string;
  if (!is_user_vaddr(string) || strnlen(string, delta) == delta)
    syscall_exit(-1);
}

/* Adds a mapping from user virtual address UPAGE to kernel
   virtual address KPAGE to the page table.
   If WRITABLE is true, the user process may modify the page;
   otherwise, it is read-only.
   UPAGE must not already be mapped.
   KPAGE should probably be a page obtained from the user pool
   with palloc_get_page().
   Returns true on success, false if UPAGE is already mapped or
   if memory allocation fails. */
static bool install_page(void* upage, void* kpage, bool writable) {
  struct thread* t = thread_current();

  /* Verify that there's not already a page at that virtual
     address, then map our page there. */
  return (pagedir_get_page(t->pagedir, upage) == NULL &&
          pagedir_set_page(t->pagedir, upage, kpage, writable));
}

static int syscall_open(const char* filename) {
  struct thread* t = thread_current();
  if (t->open_file != NULL)
    return -1;

  t->open_file = filesys_open(filename);
  if (t->open_file == NULL)
    return -1;

  return 2;
}

static int syscall_write(int fd, void* buffer, unsigned size) {
  struct thread* t = thread_current();
  if (fd == STDOUT_FILENO) {
    putbuf(buffer, size);
    return size;
  } else if (fd != 2 || t->open_file == NULL)
    return -1;

  return (int)file_write(t->open_file, buffer, size);
}

static int syscall_read(int fd, void* buffer, unsigned size) {
  struct thread* t = thread_current();
  if (fd != 2 || t->open_file == NULL)
    return -1;

  return (int)file_read(t->open_file, buffer, size);
}

static void syscall_close(int fd) {
  struct thread* t = thread_current();
  if (fd == 2 && t->open_file != NULL) {
    file_close(t->open_file);
    t->open_file = NULL;
  }
}

static void* syscall_sbrk(intptr_t increment) {
  //keep track of start of heap
  struct thread* t = thread_current();
  int old_break = t->seg_break;
  if (increment == 0) {
    return old_break;
  } else {
    //the start of the next page to be allocated
    
    if (increment > 0) {
      uint32_t cur_pg_start = pg_round_up(t->seg_break);
      
      uint32_t last_pg_start = pg_round_up(t->seg_break + increment);
      for (int i=cur_pg_start; i<last_pg_start; i+=PGSIZE) {
        //allocate a page. uses similar format to the setup_stack example
        uint8_t* kpage;
        bool success = false;

        kpage = palloc_get_page(PAL_USER | PAL_ZERO);
        if (kpage != NULL) {
          if (!install_page(i, kpage, true)) {
            palloc_free_page(kpage);
            //free other pages allocated by this sbrk
            for (int j=cur_pg_start; j<i; j+= PGSIZE) {
              uint8_t* old_kpage = pagedir_get_page(t->pagedir, j);
              pagedir_clear_page(t->pagedir, j);
              if (old_kpage != NULL) {
                palloc_free_page(old_kpage);
              }
            }
            return -1;
          }
        } else {
          //free other pages allocated by this sbrk
          for (int j=cur_pg_start; j<i; j+= PGSIZE) {
            uint8_t* old_kpage = pagedir_get_page(t->pagedir, j);
            pagedir_clear_page(t->pagedir, j);
            if (old_kpage != NULL) {
              palloc_free_page(old_kpage);
            }
          }
          return -1;
        }
      }
    } else {
      //add -1 because seg_break should not be included (if seg_break is on boundary)
      //this gets the beginning of what is allocated, and then what should be allocated
      uint32_t cur_pg_start = pg_round_down(t->seg_break - 1);
      uint32_t last_pg_start = pg_round_down(t->seg_break + increment - 1);
      //free pages, segment break goes down
      //if cur is equal to the last, nothing should happen
      for (int i=cur_pg_start; i>last_pg_start; i-=PGSIZE) {
        //allocate a page. uses similar format to the setup_stack example
        uint8_t* kpage = pagedir_get_page(t->pagedir, i);
        pagedir_clear_page(t->pagedir, i);
        if (kpage != NULL) {
          palloc_free_page(kpage);
        }
      }
    }
  }
  t->seg_break += increment;
  return old_break;
}

static void syscall_handler(struct intr_frame* f) {
  uint32_t* args = (uint32_t*)f->esp;
  struct thread* t = thread_current();
  t->in_syscall = true;

  validate_buffer_in_user_region(args, sizeof(uint32_t));
  switch (args[0]) {
    case SYS_EXIT:
      validate_buffer_in_user_region(&args[1], sizeof(uint32_t));
      syscall_exit((int)args[1]);
      break;

    case SYS_OPEN:
      validate_buffer_in_user_region(&args[1], sizeof(uint32_t));
      validate_string_in_user_region((char*)args[1]);
      f->eax = (uint32_t)syscall_open((char*)args[1]);
      break;

    case SYS_WRITE:
      validate_buffer_in_user_region(&args[1], 3 * sizeof(uint32_t));
      validate_buffer_in_user_region((void*)args[2], (unsigned)args[3]);
      f->eax = (uint32_t)syscall_write((int)args[1], (void*)args[2], (unsigned)args[3]);
      break;

    case SYS_READ:
      validate_buffer_in_user_region(&args[1], 3 * sizeof(uint32_t));
      validate_buffer_in_user_region((void*)args[2], (unsigned)args[3]);
      f->eax = (uint32_t)syscall_read((int)args[1], (void*)args[2], (unsigned)args[3]);
      break;

    case SYS_CLOSE:
      validate_buffer_in_user_region(&args[1], sizeof(uint32_t));
      syscall_close((int)args[1]);
      break;
    case SYS_SBRK:
      validate_buffer_in_user_region(&args[1], sizeof(intptr_t));
      f->eax = (uint32_t) syscall_sbrk((int)args[1]);
      break;
    default:
      printf("Unimplemented system call: %d\n", (int)args[0]);
      break;
  }

  t->in_syscall = false;
}
