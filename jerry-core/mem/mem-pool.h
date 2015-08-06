/* Copyright 2014-2015 Samsung Electronics Co., Ltd.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef JERRY_MEM_POOL_INTERNAL
#error "Please, use mem_poolman.h instead of mem_pool.h"
#endif

#ifndef JERRY_MEM_POOL_H
#define JERRY_MEM_POOL_H

#include "mem-config.h"
#include "mem-heap.h"

/** \addtogroup pool Memory pool
 * @{
 */

/**
 * Size of a pool (header + chunks)
 */
#define MEM_POOL_SIZE \
  ((size_t) JERRY_MIN ((1ull << MEM_POOL_MAX_CHUNKS_NUMBER_LOG) * MEM_POOL_CHUNK_SIZE, \
                       JERRY_ALIGNDOWN (mem_heap_recommend_allocation_size (CONFIG_MEM_LEAST_CHUNK_NUMBER_IN_POOL * \
                                                                            MEM_POOL_CHUNK_SIZE), \
                                        MEM_POOL_CHUNK_SIZE)))

/**
 * Number of chunks in a pool
 */
#define MEM_POOL_CHUNKS_NUMBER ((MEM_POOL_SIZE) / MEM_POOL_CHUNK_SIZE)

/**
 * Get pool's space size
 */
#define MEM_POOL_SPACE_START(pool_header_p) ((uint8_t*) pool_header_p)

/**
 * Index of chunk in a pool
 */
typedef uint8_t mem_pool_chunk_index_t;

typedef uint64_t mem_pool_chunk_t;

/**
 * State of a memory pool
 */
typedef struct mem_pool_state_t
{
} mem_pool_state_t;

extern mem_pool_chunk_t * mem_pool_init (mem_pool_state_t *pool_p, size_t pool_size);
extern uint8_t* mem_pool_alloc_chunk (mem_pool_state_t *pool_p);
extern void mem_pool_free_chunk (mem_pool_state_t *pool_p, uint8_t *chunk_p);
extern bool __attr_const___ mem_pool_is_chunk_inside (mem_pool_state_t *pool_p, uint8_t *chunk_p);

/**
 * @}
 */

#endif /* JERRY_MEM_POOL_H */
