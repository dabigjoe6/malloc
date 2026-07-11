#include <stdio.h>
#include <sys/mman.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <stdlib.h>
#include <assert.h>

static char *my_addr;
static const size_t CHUNK_SIZE = 2 * 1024 * 1024; // 2MB

static const size_t WORD = 4; // 1 WORD = 4 bytes 
static const size_t DWORD = 2 * WORD;  // Double WORD = 8 bytes

static uint64_t *prologue_hdr;
static uint64_t *epilogue_hdr;

uint64_t *get_free_block(size_t requested_block_size);
uint64_t *get_free_block_and_coalesce(uint64_t *free_block, size_t requested_size);
uint64_t *my_malloc(size_t payload_size);
size_t get_block_size(uint64_t *block_hdr_or_footer);
int still_in_heap(uint64_t *block_hdr);
int is_allocated_block(uint64_t *block);
int my_malloc_init();
int my_free(uint64_t *payload_address);
void check_heap();
int compare_nanoseconds_asc(const void *a, const void *b);


int compare_nanoseconds_asc(const void *a, const void *b) {
  uint64_t time1 = *(const uint64_t *)a;
  uint64_t time2 = *(const uint64_t *)b;

  return (time1 > time2) - (time1 < time2); 
}

uint64_t *my_malloc(size_t payload_size) {
  size_t min_size_before_alignment = 16 + payload_size; // total block size -- (header + footer -- 8 bytes each) + payload

  // to keep 16 bytes alignment
  // 16, 32, 48... all have their list significant 4 bits unset - 0000
  // 16 = 0001 0000
  // 32 = 0010 0000
  // 48 = 0011 0000

  // so for alignment we use what is called a "pre-add" i.e 
  // the smallest number we can possibly add to our input 
  // such that clearing the last 4 bits will round down to a multiple of 16

  // if our input is 16,
  // 16 + 15 = 31, which is just shy of 32 a multiple of 16 but we round down back to 16 without any "padding"
  // 15 is the smallest number we can add to keep alignment with 16, this is our "pre-add"

  // if our input is 24 i.e it is between 16 and 32
  // 24 + 15 = 39, which is rounded down back to 32 when we clear the least significant 4 bits

  // 15 in binary is 0000 1111
  // ~15 is our mask which helps clear the least significant bit we care about

  size_t aligned_total_size = (min_size_before_alignment + 15) & ~15;
  
  // find a free block or create one with the aligned_total_size
  // passing an aligned size downstream to make alignment easier
  // all functions downstream from here assumes the size it receives is 16 bytes aligned
  uint64_t *free_block = get_free_block(aligned_total_size);

  // return user first payload pointer/address
  assert((((uintptr_t)(free_block + 1) & 15) == 0) && "expected payload address to be 16 bytes aligned");
  return free_block + 1;
}

uint64_t *get_free_block(size_t requested_block_size) {
  // starts looking just after the footer of the prologue
  uint64_t *current_block = prologue_hdr + 2;
  assert(((uintptr_t)current_block & 7) == 0 && "expected free block header to be 8 bytes aligned");

  while (still_in_heap(current_block)) {
    if (is_allocated_block(current_block) || get_block_size(current_block) < requested_block_size) {
      current_block += (get_block_size(current_block) / 8);
      continue;
    }

    if (get_block_size(current_block) == requested_block_size) {
      // set allocated bit before returning block
      // block header
      *current_block |= 0x1;
      // block footer
      *(current_block + (requested_block_size / 8) - 1) |= 0x1;
      
      return current_block;
    } else if (get_block_size(current_block) > requested_block_size) {
      return get_free_block_and_coalesce(current_block, requested_block_size);
    }
  }

  return NULL; // at this point there was no free block and got to the epilogue // TODO: Handle getting more memory?
}

uint64_t *get_free_block_and_coalesce(uint64_t *free_block, size_t requested_size) {
  size_t free_block_size = get_block_size(free_block);

  size_t coalesced_block_size = free_block_size - requested_size;

  // free_block_size is expected to be 16 bytes aligned and the requested size passed to
  // get_free_block_and_coalesce is also expected to be 16 byte aligned
  // so the coalesced_block_size is also expected to be 16 byte aligned

  assert(((free_block_size & 15) == 0) && "expected free block size in get_free_block_and_coalesce to be 16 bytes aligned");
  assert(((requested_size & 15) == 0) && "expected requested block size in get_free_block_and_coalesce to be 16 bytes aligned");
  assert(((coalesced_block_size & 15) == 0) && "expected coalesced block size in get_free_block_and_coalesce to be 16 bytes aligned");
  
  if (free_block_size > requested_size && coalesced_block_size % 16 == 0) {
    // coalesced header
    uint64_t *coalesced_block_hdr = free_block + (requested_size / 8);
    *coalesced_block_hdr = 0x0;
    *coalesced_block_hdr |= (coalesced_block_size << 4);

    // coalesced footer
    uint64_t *coalesced_block_footer = coalesced_block_hdr + ((coalesced_block_size / 8) - 1);
    *coalesced_block_footer = 0x0;
    *coalesced_block_footer |= (coalesced_block_size << 4);

    // allocate and set free block size before returning the free block
    
    // free block header
    *free_block = 0x1;
    *free_block |= (requested_size << 4);

    // free block footer
    uint64_t *free_block_footer = free_block + ((requested_size / 8) - 1);
    *free_block_footer = 0x1;
    *free_block_footer |= (requested_size << 4);

    return free_block;
  } else {  
    // I actually don't think we will ever get into this block // TODO: Verify
    
    // when the remaining space is not enough to be 16 bytes aligned just return the entire free block
    // set free block header's allocated bit
    *free_block |= 0x1;

    // set free block footer's allocated bit
    *(free_block + ((free_block_size / 8) - 1)) |= 0x1;

    return free_block;
  }
}

size_t get_block_size(uint64_t *block_hdr_or_footer) {
  return *block_hdr_or_footer >> 4;
}

int still_in_heap(uint64_t *block_hdr) {
  if (get_block_size(block_hdr) == 0) return 0;

  return 1;
}

int is_allocated_block(uint64_t *block) {
  return *block & 0x1;
}

int my_malloc_init() {
  uint64_t *mmap_addr = (uint64_t *) mmap(
    NULL, 
    CHUNK_SIZE, 
    PROT_READ | PROT_WRITE, 
    MAP_PRIVATE | MAP_ANONYMOUS,
    -1, 0); 

  if (mmap_addr == MAP_FAILED) {
    perror("mmap failed");
    return 1;
  }

  assert((((uintptr_t) mmap_addr) & 15) == 0 && "expected address returned from mmap to be 16 bytes aligned");

  prologue_hdr = mmap_addr + 1; // the +1 is needed to offset the prologue_hdr by 8 bytes so 
                // eventually the first user payload block lands on a 16 byte aligned address
                // --- mmap returns this address
                // --- next 8 byte prologue header starts here
                // --- next 16 byte prologue footer starts here
                // --- next 24 byte free block header starts here
                // --- next 32 byte free block payload starts here and is 16 bytes aligned

  assert((((uintptr_t)(prologue_hdr + 1) & 15) == 0) && "expected prologue footer to be 16 bytes aligned");

  // create prologue
  // set prologues header value and allocated bit
  *prologue_hdr = 0x1;
  *prologue_hdr |= ((2 * DWORD) << 4);  

  // create prologues footer
  *(prologue_hdr + 1) = 0x1;
  *(prologue_hdr + 1) |= ((2 * DWORD) << 4);

  size_t ACTUAL_CHUNK_SIZE = CHUNK_SIZE - 8; // remove offset from chunk size

  

  //create initial free block
  size_t free_block_size = ACTUAL_CHUNK_SIZE - (16 + 8);
                         //ACTUAL_CHUNK_SIZE - (prologue_size (8 bytes header + 8 bytes footer) + 
                         //                     epilogue_size (8 bytes header))
  
  size_t aligned_free_block_size = (free_block_size + 15) & ~15; // keep 16 bytes alignment of free block size
  assert((aligned_free_block_size & 15) == 0 && "expected free block size to be 16 bytes aligned");

  uint64_t *free_block_hdr = prologue_hdr + 2;

  assert((((uintptr_t)(free_block_hdr + 1) & 15) == 0) && "expected payload address to be 16 bytes aligned");

  *free_block_hdr = 0x0;
  *free_block_hdr |= (aligned_free_block_size << 4);

  uint64_t *free_block_footer = free_block_hdr + ((aligned_free_block_size / 8) - 1);
  *free_block_footer = 0x0;
  *free_block_footer |= (aligned_free_block_size << 4);
  
  // epilogue header
  epilogue_hdr = free_block_hdr + (aligned_free_block_size / 8);
  *epilogue_hdr = 0x1;
  *epilogue_hdr |= (0 << 4);

  assert((((uintptr_t)epilogue_hdr & 7) == 0) && "expected epilogue header address to be 8 bytes aligned");

  return 0;
}

int my_free(uint64_t *payload_address) {
  int is_prev_block_free;
  int is_next_block_free;

  assert((((uintptr_t)payload_address & 15) == 0) && "expected payload block address to be 16 bytes aligned");

  // assuming user sends address of first payload
  uint64_t *block_hdr = payload_address - 1;
  uint64_t *block_footer = block_hdr + ((get_block_size(block_hdr) / 8)  - 1); 

  is_prev_block_free = (*(block_hdr - 1) & 0x1) == 0;
  is_next_block_free = (*(block_footer + 1) & 0x1) == 0;

  size_t prev_block_size = *(block_hdr - 1) >> 4;
  size_t next_block_size = *(block_footer + 1) >> 4;
  size_t block_size = *block_hdr >> 4;

  if (is_prev_block_free && is_next_block_free) {
    block_hdr = block_hdr - (prev_block_size / 8);
    block_footer = block_footer + (next_block_size / 8);

    block_size = prev_block_size + block_size + next_block_size;
    // I would expect the new size to still be 16 bytes aligned
    assert(((block_size & 15) == 0) && "expected new block size (prev + block + next) to be 16 bytes aligned");
  } else if (is_prev_block_free) {
    block_hdr = block_hdr - (prev_block_size / 8);

    block_size = prev_block_size + block_size;
    assert(((block_size & 15) == 0) && "expected new block size (prev + block) to be 16 bytes aligned");
  } else if (is_next_block_free) {
    block_footer = block_footer + (next_block_size / 8);

    block_size = block_size + next_block_size;
    assert(((block_size & 15) == 0) && "expected new block size (block + next) to be 16 bytes aligned");
  }

  // free from existing chunk
  // mark as free
  *block_hdr = 0x0;
  *block_hdr |= (block_size << 4);

  *block_footer = 0x0;
  *block_footer |= (block_size << 4);

  return 0;
}

void check_heap() {
  // prologue is always allocated and the size is correct
  assert((get_block_size(prologue_hdr) == 16)
              && "expected prologue size to be 16 bytes");
  assert((get_block_size(prologue_hdr) == get_block_size(prologue_hdr + 1))
              && "expected size stored in prologue header to be the same as size stored in prologue footer");
  assert((is_allocated_block(prologue_hdr) == 1) && "expected prologue header to be allocated");
  assert(is_allocated_block(prologue_hdr + 1) == 1 && "expected prologue footer to be allocated");
  assert((((uintptr_t)prologue_hdr & 7) == 0) && "expected prologue header address to be 8 bytes aligned");

  uint64_t *current_block = prologue_hdr + 2;

  while (still_in_heap(current_block)) {
    // payload starts on a 16 byte aligned address
    assert((((uintptr_t)(current_block + 1) & 15) == 0) && "expected payload address to be 16 bytes aligned");
      
    // every block size is a factor of 16
    assert(((get_block_size(current_block) & 15) == 0) && "expected block size to be a multiple of 16");

    // every block has the same metadata (size and allocated bit) in the header and footer
    uint64_t *current_block_footer = current_block + ((get_block_size(current_block) / 8) - 1); 
    assert(get_block_size(current_block) == get_block_size(current_block_footer)
              && "expected size stored in block header to be the same as size stored in footer");
    assert(is_allocated_block(current_block) == is_allocated_block(current_block_footer)
              && "expected allocated bit stored in block header to be the same as allocated bit stored in footer");

    // TODO: Check no block overlaps (not sure how to implement this or if it makes sense to even try, 
    // I need to think of it)
    
    current_block = current_block_footer + 1;
  }

  // epilogue is allocated and the size is 0
  assert(get_block_size(current_block) == 0 && "expected block to be the epilogue and block size to be 0");
  assert(is_allocated_block(current_block) == 1 && "expected epilogue block to be allocated");
}

int harness() {
  // list of malloc addresses - [m_1, m_2, m_3, m_4]

  // malloc_counter starts at 0
  // max_malloc_count = 100 to start with?

  // random op each loop -> malloc or free

  // when random picked malloc
  // so malloc a random size
  // increment malloc_counter

  // or
  // random picked free
  // so free a random payload address from a list of payload adresses
  // decrement malloc_counter

  struct timespec start, end;
  
  const int max_malloc_count = 100;
  int malloc_counter = 0;
  int free_counter = 0;

  uint64_t *payloads[max_malloc_count];
  memset(payloads, 0, sizeof(payloads));

  uint64_t *malloc_payloads[max_malloc_count];
  memset(malloc_payloads, 0, sizeof(malloc_payloads));

  uint64_t my_malloc_latencies[max_malloc_count];
  uint64_t my_free_latencies[max_malloc_count];

  uint64_t malloc_latencies[max_malloc_count];
  uint64_t free_latencies[max_malloc_count];

  memset(my_malloc_latencies, 0, sizeof(my_malloc_latencies));
  memset(my_free_latencies, 0, sizeof(my_free_latencies));

  memset(malloc_latencies, 0, sizeof(malloc_latencies));
  memset(free_latencies, 0, sizeof(free_latencies));
  
  srand(time(NULL));

  my_malloc_init();
  
  do {
    int malloc_or_free = rand() % 2;

    if (malloc_or_free == 1) { 
      // malloc
      size_t random_size = (size_t)((rand() % 2048) + 1);

      clock_gettime(CLOCK_MONOTONIC, &start);
      uint64_t *payload_address = my_malloc(random_size);
      clock_gettime(CLOCK_MONOTONIC, &end);

      uint64_t my_delta_ns = (((uint64_t)end.tv_sec * 1000000000ULL) + (uint64_t)end.tv_nsec) - 
                            (((uint64_t)start.tv_sec * 1000000000ULL) + (uint64_t)start.tv_nsec);

      clock_gettime(CLOCK_MONOTONIC, &start);
      uint64_t *malloc_payload_address = (uint64_t*) malloc(random_size);
      clock_gettime(CLOCK_MONOTONIC, &end);

      uint64_t delta_ns = (((uint64_t)end.tv_sec * 1000000000ULL) + (uint64_t)end.tv_nsec) - 
                            (((uint64_t)start.tv_sec * 1000000000ULL) + (uint64_t)start.tv_nsec);

      if (payload_address == NULL) {
        printf("failed to allocate size %zu bytes\n", random_size);
        return 1;
      } else {
        size_t allocated_size = get_block_size(payload_address - 1);
        printf("requested size %zu bytes, allocated %zu bytes\n", random_size, allocated_size);
        payloads[malloc_counter] = payload_address;
        my_malloc_latencies[malloc_counter] = my_delta_ns;  
        
        if (malloc_payload_address != NULL) {
          malloc_payloads[malloc_counter] = malloc_payload_address; 
          malloc_latencies[malloc_counter] = delta_ns;
        }

        malloc_counter += 1;
      }
    } else {
      // free
      int payload_to_free = rand() % max_malloc_count;
      uint64_t *payload_address_to_free = payloads[payload_to_free];
      uint64_t *payload_address_to_free_from_real_free = malloc_payloads[payload_to_free];
      
      if (payload_address_to_free != NULL) {
        size_t payload_size = get_block_size(payload_address_to_free - 1);

        clock_gettime(CLOCK_MONOTONIC, &start);
        int is_free = my_free(payload_address_to_free);
        clock_gettime(CLOCK_MONOTONIC, &end);

        uint64_t my_delta_ns = (((uint64_t)end.tv_sec * 1000000000ULL) + (uint64_t)end.tv_nsec) - 
                              (((uint64_t)start.tv_sec * 1000000000ULL) + (uint64_t)start.tv_nsec);

        clock_gettime(CLOCK_MONOTONIC, &start);
        free(payload_address_to_free_from_real_free);
        clock_gettime(CLOCK_MONOTONIC, &end);

        uint64_t delta_ns = (((uint64_t)end.tv_sec * 1000000000ULL) + (uint64_t)end.tv_nsec) - 
                              (((uint64_t)start.tv_sec * 1000000000ULL) + (uint64_t)start.tv_nsec);

        if (is_free != 0) {
          printf("failed to free size %zu bytes at address %p\n", payload_size, (void*)payload_address_to_free);
          return 1;
        } else {
          printf("freed %zu bytes at address %p\n", payload_size, (void*)payload_address_to_free);
          payloads[payload_to_free] = NULL;
          my_free_latencies[free_counter] = my_delta_ns;

          malloc_payloads[payload_to_free] = NULL;
          free_latencies[free_counter] = delta_ns;
          
          free_counter += 1;
        }
      } else {
        printf("no address to free\n");
      }
    }

    check_heap();
  } while (malloc_counter < max_malloc_count);

  qsort(my_malloc_latencies, max_malloc_count, sizeof(uint64_t), compare_nanoseconds_asc);
  qsort(malloc_latencies, max_malloc_count, sizeof(uint64_t), compare_nanoseconds_asc);

  qsort(my_free_latencies, free_counter, sizeof(uint64_t), compare_nanoseconds_asc);
  qsort(free_latencies, free_counter, sizeof(uint64_t), compare_nanoseconds_asc);

  uint64_t total_my_malloc_latency = 0;
  uint64_t total_malloc_latency = 0;

  uint64_t total_my_free_latency = 0;
  uint64_t total_free_latency = 0;

  for (int i = 0; i < malloc_counter; ++i) {
    total_my_malloc_latency += my_malloc_latencies[i];
    total_malloc_latency += malloc_latencies[i];
  }

  for (int i = 0; i < malloc_counter; ++i) {
    total_my_free_latency += my_free_latencies[i];
    total_free_latency += free_latencies[i];
  }

  const double NS_PER_SEC = 1000000000.0;

  double total_my_malloc_latency_seconds = (double)total_my_malloc_latency / NS_PER_SEC;
  double total_malloc_latency_seconds = (double)total_malloc_latency / NS_PER_SEC;

  double total_my_free_latency_seconds = (double)total_my_free_latency / NS_PER_SEC;
  double total_free_latency_seconds = (double)total_free_latency / NS_PER_SEC;

  uint64_t my_malloc_throughput = (uint64_t)((double)malloc_counter / total_my_malloc_latency_seconds);
  uint64_t malloc_throughput = (uint64_t)((double)malloc_counter / total_malloc_latency_seconds);

  uint64_t my_free_throughput = (uint64_t)((double)free_counter / total_my_free_latency_seconds);
  uint64_t free_throughput = (uint64_t)((double)free_counter / total_free_latency_seconds);

  // interestingly here i learnt about rounding up using Integer arithmetic
  // Which is way faster than using something like .ceil due to float conversion and from math.h
  // formula for integer roundup is 
  //              Ceiling (A/B) = A + B - 1
  //                             -----------
  //                                  B
  // formula for getting the index from the percentile is basically
  // p50, ... p90, ... p_x
  // p50_index =  ceiling(n * 0.5) - 1
  // p90_index = ceiling(n * 0.99) - 1
  //
  // so for p50 -> 0.5 -> 1/2 -> ceiling(A/B) -> integer arithmetic folumar above for ceiling
  int malloc_p50_index = ((max_malloc_count + 1) / 2) - 1;
  int malloc_p99_index = ((((max_malloc_count * 99) + 99) / 100)) - 1;

  int free_p50_index = ((free_counter + 1) / 2) - 1;
  int free_p99_index = ((((free_counter * 99) + 99) / 100)) - 1;

  printf("\n====== Malloc ======\n");
  printf("[Throughput] my_malloc: %lldops/s\n", my_malloc_throughput);
  printf("[Throughput] malloc:    %lldops/s\n\n",    malloc_throughput);

  printf("[p99] my_malloc: %lldns\n", my_malloc_latencies[malloc_p99_index]);
  printf("[p99] malloc:    %lldns\n\n", malloc_latencies[malloc_p99_index]);

  printf("[p50] my_malloc: %lldns\n", my_malloc_latencies[malloc_p50_index]);
  printf("[p50] malloc:    %lldns\n\n",    malloc_latencies[malloc_p50_index]);
  
  printf("====== Free ======\n");
  printf("[Throughput] my_free: %lldops/s\n", my_free_throughput);
  printf("[Throughput] free:    %lldops/s\n\n",    free_throughput);

  printf("[p99] my_free: %lldns\n", my_free_latencies[free_p99_index]);
  printf("[p99] free:    %lldns\n\n",    free_latencies[free_p99_index]);

  printf("[p50] my_free: %lldns\n", my_free_latencies[free_p50_index]);
  printf("[p50] free:    %lldns\n\n", free_latencies[free_p50_index]);
  return 0;
}

int main(int argc, char *argv[]) {

  harness();

  return 0;
}
