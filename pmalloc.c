#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdbool.h>
#include <string.h>
#include <sys/mman.h>
#include <assert.h>

#define OS_MEM_ALLOC_SIZE (4096 * 1024)

#define PM_MAGIC (0xBADCAFFE)

/*
 *
 * Head ----> [BLK_HDR | USER_DATA1] <--> [BLK_HDR | USER_DATA2] <--> [BLK_HDR | USER_DATAn]
 *
 */

/*
 * Pmalloc: A poor mans malloc/free implementation. Basic idea is to use a
 * doubly linked list of free blocks. When ever we allocate a block we will
 * remove from the chain. When we eventually free this block it will be added
 *  back again
 */


struct pm_block {
    size_t size; 				// Size of user data
    struct pm_block* next;
    struct pm_block* prev;
    unsigned int magic;
    char user_data[0];
};
typedef struct pm_block pm_block_t;

static pm_block_t* g_free_list_head = NULL;	// global list of free mem blocks
static long g_reuse_count;

static inline size_t blk_hdr_size() {
    return sizeof (pm_block_t);
}

static inline void* blk_to_user_data(pm_block_t *blk) {
   assert(blk);
   return (void *) (++blk);
}

static inline pm_block_t* user_data_to_blk(void* ptr) {
   assert(ptr);
   pm_block_t* blk = (pm_block_t *)ptr;
   return (--blk);
}

FILE* debug_stream() {
    static FILE* fp;
    if (!fp) {
        fp = fopen("pm_log", "w+");
        assert(fp);
    }

    return fp;
}

enum FREE_LIST_OP {REMOVE, ADD, TRAVERSE};

// Use system call to allocate an Anonymous memory that we can carve out in to smaller pm_blocks
static void* get_mem_from_os(size_t size) {
    void* mem = mmap(0, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    assert(mem);
    return mem;
}

// Add or Remove a block to freelist. When a block is returned to user we will remove
// and when block is freed by user we will add to free list
void free_list_op(pm_block_t *blk, enum FREE_LIST_OP op) {
    assert(blk != NULL);
    switch (op) {
    case REMOVE:
    {
        // Step 1: Traverse to the blk
        // Step 2 : If its Head then update head
        // Step 3: Intermediate block
        pm_block_t* itr = g_free_list_head;
        assert(itr);

        if (blk == g_free_list_head) {
            g_free_list_head = blk->next;
            return;
        }
        while (itr) {
            if (itr == blk) {
                pm_block_t* prev_blk = itr->prev;
                pm_block_t* next_blk = itr->next;
                prev_blk->next = next_blk;
                if (next_blk) {
                    next_blk->prev = prev_blk;
                }
                return;
            }
            itr = itr->next;
        }
    }
        break;
    case ADD:
    {
        // Step 1: If Global free list is empty.
        // 	Step 1a: Update head and insert blk
        // Step 2: Push to Head that would be O(1)
        if (!g_free_list_head) {
            g_free_list_head = blk;
            blk->next = NULL;
            blk->prev = NULL;
        } else {
            g_free_list_head->prev = blk;
            blk->next = g_free_list_head;
            g_free_list_head = blk;
        }
    }
        break;
    case TRAVERSE:
    {
        pm_block_t* itr = blk;
        int n = 0;
        while (itr) {
            fprintf(debug_stream(), "pm: [freeblk no. %d] [size %lu]\n", n, itr->size);
            n++;
            itr = itr->next;
        }

    }
        break;
    default:
        assert(0);
    }
}
// Carve a large block to smaller block of size s and return the remaining free block
pm_block_t* trim_block(pm_block_t* blk, size_t s) {
  // We want to ensure that we have atleast blk_hdr + 1 bytes worth of
  // remaining space if we trim this block.
  size_t remaining = blk->size - s;
  if (remaining <  blk_hdr_size() + 1) {
    return NULL;
  }

  pm_block_t *free_blk = (pm_block_t *)(blk->user_data + s);
  free_blk->size = blk->size - (blk_hdr_size() + s);
  // some sanity check
  assert(free_blk->size > 0);
  assert(free_blk->size <= OS_MEM_ALLOC_SIZE);

  // trim blk to new size
  blk->size = s - blk_hdr_size();
  blk->magic = PM_MAGIC;
  free_blk->magic = PM_MAGIC;
  free_blk->prev = NULL;
  free_blk->next = NULL;

  return free_blk;
}

void* pm_malloc(size_t size) {
    // Step 1: Traverse free list
    // 	Step 1a: We get a free block >= to size
    //		Step 1a.1 If size == free block then remove this free block from free list and return
    //		Step 1a.2 If size > free block then carve the block and add the remaining to free list
    //	Step 1b: No free Block available
    //		Step 1b.1 If req size > OS_MEM_ALLOC_SIZE then call get_mem_from_os with this large size
    //		Step 1b.2 Get a OS_MEM_ALLOC_SIZE and perform Step 1a.2

    // Edge condition: when size is invalid we need to return NULL
    if (size <= 0) {
        return NULL;
    }
    pm_block_t* blk_itr = g_free_list_head;
    size_t real_size = size + blk_hdr_size(); // I need to possibly request an alligned size

    while (blk_itr != NULL) {
        if (blk_itr->size >= real_size) {
                if (blk_itr->size == real_size) {
                    // Happy case, just remove from free list
                    free_list_op(blk_itr, REMOVE);
                    g_reuse_count++;
                    return blk_to_user_data(blk_itr);
                } else {
                    // Trim the larger block
                    free_list_op(blk_itr, REMOVE);
                    g_reuse_count++;
                    pm_block_t* free_blk = trim_block(blk_itr, real_size);
                    if (free_blk) {
                      free_list_op(free_blk, ADD);
                    }
                    return blk_to_user_data(blk_itr);
                }
        }

        blk_itr = blk_itr->next;
    }

    // No usable free blk
    if (real_size > OS_MEM_ALLOC_SIZE) {
        pm_block_t* large_blk = get_mem_from_os(real_size);
        large_blk->next = NULL;
        large_blk->prev = NULL;
        large_blk->magic = PM_MAGIC;
        large_blk->size = real_size - blk_hdr_size();
        return blk_to_user_data(large_blk);
    }

    // Allocate mem from OS and trim it
    // Add the new free block to freelist
    pm_block_t* alloc_blk = get_mem_from_os(OS_MEM_ALLOC_SIZE);
    alloc_blk->next = NULL;
    alloc_blk->prev = NULL;
    alloc_blk->magic = PM_MAGIC;
    alloc_blk->size = OS_MEM_ALLOC_SIZE - blk_hdr_size();
    pm_block_t* free_blk = trim_block(alloc_blk, real_size);
    if (free_blk) {
      free_list_op(free_blk, ADD);
    }

    return blk_to_user_data(alloc_blk);
}

void pm_free(void *ptr) {
    // Edge case when user asks to free a NULL block
    if (!ptr) {
        return;
    }
    pm_block_t* blk = user_data_to_blk(ptr);
    assert(blk->magic == PM_MAGIC);
    free_list_op(blk, ADD);
}

void pm_debug(const char* MSG) {
  fprintf(debug_stream(), "pm: [%s] \n", MSG);
  free_list_op(g_free_list_head, TRAVERSE);
}

void* malloc(size_t s) {
  return pm_malloc(s);
}

void free(void* ptr) {
  pm_free(ptr);
}

int main () {
  void *arr[10];

  while (1) {
    for (int i = 0; i < 10; i++) {
      size_t s = rand() % (1024 * 1024);
      arr[i] = pm_malloc(s);
      char* bufp = arr[i];
      for (int k = 0; k < s; k++) {
        bufp[k] = 'A';
      }
    }

    pm_debug("POST ALLOC");
    for (int i = 0; i < 10; i++) {
      pm_free(arr[i]);
    }
    pm_debug("POST DEALLOC");
    fprintf(debug_stream(), "pm: reused blocks [%lu]\n", g_reuse_count);
  }

  return 0;
}
