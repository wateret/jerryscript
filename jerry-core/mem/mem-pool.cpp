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

/** \addtogroup mem Memory allocation
 * @{
 *
 * \addtogroup pool Memory pool
 * @{
 */

/**
 * Memory pool implementation
 */

#define JERRY_MEM_POOL_INTERNAL

#include "jrt.h"
#include "jrt-libc-includes.h"
#include "mem-allocator.h"
#include "mem-pool.h"

/*
 * Valgrind-related options and headers
 */
#ifdef JERRY_VALGRIND
# include "memcheck.h"

# define VALGRIND_NOACCESS_SPACE(p, s)  (void)VALGRIND_MAKE_MEM_NOACCESS((p), (s))
# define VALGRIND_UNDEFINED_SPACE(p, s) (void)VALGRIND_MAKE_MEM_UNDEFINED((p), (s))
# define VALGRIND_DEFINED_SPACE(p, s)   (void)VALGRIND_MAKE_MEM_DEFINED((p), (s))
#else /* JERRY_VALGRIND */
# define VALGRIND_NOACCESS_SPACE(p, s)
# define VALGRIND_UNDEFINED_SPACE(p, s)
# define VALGRIND_DEFINED_SPACE(p, s)
#endif /* JERRY_VALGRIND */

/**
 * Get address of pool chunk with specified index
 */
#define MEM_POOL_CHUNK_ADDRESS(pool_header_p, chunk_index) ((mem_pool_chunk_t*) (MEM_POOL_SPACE_START(pool_p) + \
                                                                                 MEM_POOL_CHUNK_SIZE * chunk_index))

/**
 * Is the chunk is inside of the pool?
 *
 * @return true / false
 */
bool __attr_const___
mem_pool_is_chunk_inside (mem_pool_state_t *pool_p, /**< pool */
                          uint8_t *chunk_p) /**< chunk */
{
  if (chunk_p >= (uint8_t*) pool_p && chunk_p < (uint8_t*) pool_p + MEM_POOL_SIZE)
  {
    JERRY_ASSERT (chunk_p >= MEM_POOL_SPACE_START (pool_p)
                  && chunk_p <= MEM_POOL_SPACE_START (pool_p) + MEM_POOL_CHUNKS_NUMBER * MEM_POOL_CHUNK_SIZE);

    return true;
  }

  return false;
} /* mem_pool_is_chunk_inside */

/**
 * Initialization of memory pool.
 *
 * Pool will be located in the segment [pool_start; pool_start + pool_size).
 * Part of pool space will be used for bitmap and the rest will store chunks.
 *
 * @return pointer first chunk in the pool's free chunks list
 */
mem_pool_chunk_t *
mem_pool_init (mem_pool_state_t *pool_p, /**< pool */
               size_t pool_size)         /**< pool size */
{
  JERRY_ASSERT (pool_p != NULL);
  JERRY_ASSERT ((size_t) MEM_POOL_SPACE_START (pool_p) % MEM_ALIGNMENT == 0);

  JERRY_STATIC_ASSERT (MEM_POOL_CHUNK_SIZE % MEM_ALIGNMENT == 0);
  JERRY_STATIC_ASSERT (MEM_POOL_MAX_CHUNKS_NUMBER_LOG <= sizeof (mem_pool_chunk_index_t) * JERRY_BITSINBYTE);
  JERRY_STATIC_ASSERT (sizeof (mem_pool_chunk_t) <= MEM_POOL_CHUNK_SIZE);
  JERRY_STATIC_ASSERT (sizeof (mem_pool_chunk_index_t) <= MEM_POOL_CHUNK_SIZE);

  /* free pool chunks contain pointers */
  JERRY_STATIC_ASSERT (sizeof (void *) <= sizeof (mem_pool_chunk_t));

  JERRY_ASSERT (MEM_POOL_SIZE == MEM_POOL_CHUNKS_NUMBER * MEM_POOL_CHUNK_SIZE);
  JERRY_ASSERT (MEM_POOL_CHUNKS_NUMBER <= (1u << CONFIG_MEM_POOL_MAX_CHUNKS_NUMBER_LOG));

  JERRY_ASSERT (pool_size == MEM_POOL_SIZE);

  /*
   * Chunk with zero index is first free chunk in the pool now
   */
  mem_pool_chunk_t *first_chunk_p = MEM_POOL_CHUNK_ADDRESS (pool_p, 0);

  mem_pool_chunk_t *prev_free_chunk_p = NULL;

  for (mem_pool_chunk_index_t chunk_index = 0;
       chunk_index < MEM_POOL_CHUNKS_NUMBER;
       chunk_index++)
  {
    mem_pool_chunk_t *chunk_p = MEM_POOL_CHUNK_ADDRESS (pool_p, chunk_index);

    if (prev_free_chunk_p != NULL)
    {
      *(mem_pool_chunk_t **) prev_free_chunk_p = chunk_p;
    }

    prev_free_chunk_p = chunk_p;
  }

  *(mem_pool_chunk_t **) prev_free_chunk_p = NULL;

  for (mem_pool_chunk_index_t chunk_index = 0;
       chunk_index < MEM_POOL_CHUNKS_NUMBER;
       chunk_index++)
  {
    /* unused if valgrind build mode is turned off */
    mem_pool_chunk_t *chunk_p __attr_unused___ = MEM_POOL_CHUNK_ADDRESS (pool_p, chunk_index);

    VALGRIND_NOACCESS_SPACE (chunk_p, MEM_POOL_CHUNK_SIZE);
  }

  return first_chunk_p;
} /* mem_pool_init */

/**
 * @}
 * @}
 */
