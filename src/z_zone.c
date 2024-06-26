/* Emacs style mode select   -*- C++ -*-
 *-----------------------------------------------------------------------------
 *
 *
 *  PrBoom: a Doom port merged with LxDoom and LSDLDoom
 *  based on BOOM, a modified and improved DOOM engine
 *  Copyright (C) 1999 by
 *  id Software, Chi Hoang, Lee Killough, Jim Flynn, Rand Phares, Ty Halderman
 *  Copyright (C) 1999-2000 by
 *  Jess Haas, Nicolas Kalkhof, Colin Phipps, Florian Schulze
 *  Copyright 2005, 2006 by
 *  Florian Schulze, Colin Phipps, Neil Stevens, Andrey Budko
 *
 *  This program is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public License
 *  as published by the Free Software Foundation; either version 2
 *  of the License, or (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
 *  02111-1307, USA.
 *
 * DESCRIPTION:
 *      Zone Memory Allocation. Neat.
 *
 * Neat enough to be rewritten by Lee Killough...
 *
 * Must not have been real neat :)
 *
 * Made faster and more general, and added wrappers for all of Doom's
 * memory allocation functions, including malloc() and similar functions.
 * Added line and file numbers, in case of error. Added performance
 * statistics and tunables.
 *-----------------------------------------------------------------------------
 */


#include "config.h"

#include <stdlib.h>

#include "z_zone.h"
#include "doomstat.h"
#include "m_argv.h"
#include "v_video.h"
#include "g_game.h"
#include "lprintf.h"

// Tunables

// Alignment of zone memory (benefit may be negated by HEADER_SIZE, CHUNK_SIZE)
#define CACHE_ALIGN 32

// Minimum chunk size at which blocks are allocated
#define CHUNK_SIZE 32

// Minimum size a block must be to become part of a split
#define MIN_BLOCK_SPLIT (1024)

// How much RAM to leave aside for other libraries
#define LEAVE_ASIDE (128*1024)

// Amount to subtract when retrying failed attempts to allocate initial pool
#define RETRY_AMOUNT (256*1024)

// signature for block header
#define ZONEID  0x931d4a11

// Number of mallocs & frees kept in history buffer (must be a power of 2)
#define ZONE_HISTORY 4

// End Tunables

typedef struct memblock {
  struct memblock *next,*prev;
  size_t size;
  unsigned char tag;

} memblock_t;

/* size of block header
 * cph - base on sizeof(memblock_t), which can be larger than CHUNK_SIZE on
 * 64bit architectures */
static const size_t HEADER_SIZE = (sizeof(memblock_t)+CHUNK_SIZE-1) & ~(CHUNK_SIZE-1);

static memblock_t *blockbytag[PU_MAX];

// 0 means unlimited, any other value is a hard limit
#ifdef MEMORY_LOW
/* Set a default limit of 16 MB; smaller values
 * will cause performance issues when rendering
 * large levels */
static int memory_size = 16*1024*1024;
/* Set a minimum 'limited' size of 8 MB */
#define MIN_MEMORY_SIZE (8*1024*1024)
#else
static int memory_size = 0;
#endif
static int free_memory = 0;


void Z_Close(void)
{
   /* The libretro core will crash on
    * close content if we free memory
    * here while running on Windows... */
#if !defined(_WIN32)
   Z_FreeTags(PU_FREE, PU_MAX);
#endif
   memory_size = 0;
   free_memory = 0;
}

bool Z_Init(void)
{
   unsigned i;
   for (i = 0; i < PU_MAX; i++)
      blockbytag[i] = NULL;

   return true;
}

/* Z_Malloc
 * You can pass a NULL user if the tag is < PU_PURGELEVEL.
 *
 * cph - the algorithm here was a very simple first-fit round-robin 
 *  one - just keep looping around, freeing everything we can until 
 *  we get a large enough space
 *
 * This has been changed now; we still do the round-robin first-fit, 
 * but we only free the blocks we actually end up using; we don't 
 * free all the stuff we just pass on the way.
 */

void *Z_Malloc(size_t size, int tag, void **user)
{
   memblock_t *block = NULL;

   if (!size)
      return user ? *user = NULL : NULL;           // malloc(0) returns NULL

   size = (size+CHUNK_SIZE-1) & ~(CHUNK_SIZE-1);  // round to chunk size

   if (memory_size > 0 && ((free_memory + memory_size) < (int)(size + HEADER_SIZE)))
   {
      memblock_t *end_block;
      block = blockbytag[PU_CACHE];
      if (block)
      {
         end_block = block->prev;
         while (1)
         {
            memblock_t *next = block->next;
            (Z_Free)((uint8_t*) block + HEADER_SIZE);
            if (((free_memory + memory_size) >= (int)(size + HEADER_SIZE)) || (block == end_block))
               break;
            block = next;               // Advance to next block
         }
      }
      block = NULL;
   }

   while (!(block = (malloc)(size + HEADER_SIZE))) {
      if (!blockbytag[PU_CACHE])
         I_Error ("Z_Malloc: Failure trying to allocate %lu bytes"
               ,(unsigned long) size
               );
      Z_FreeTags(PU_CACHE,PU_CACHE);
   }

   if (!blockbytag[tag])
   {
      blockbytag[tag] = block;
      block->next = block->prev = block;
   }
   else
   {
      blockbytag[tag]->prev->next = block;
      block->prev = blockbytag[tag]->prev;
      block->next = blockbytag[tag];
      blockbytag[tag]->prev = block;
   }

   block->size = size;

   free_memory -= block->size;

   block->tag = tag;           // tag
   block = (memblock_t *)((uint8_t*) block + HEADER_SIZE);
   if (user)                   // if there is a user
      *user = block;            // set user to point to new block

   return block;
}

void Z_Free(void *p)
{
   memblock_t *block = (memblock_t *)((uint8_t*) p - HEADER_SIZE);

   if (!p || !block)
      return;

   if (block == block->next)
      blockbytag[block->tag] = NULL;
   else
      if (blockbytag[block->tag] == block)
         blockbytag[block->tag] = block->next;
   block->prev->next = block->next;
   block->next->prev = block->prev;

   free_memory += block->size;

   (free)(block);
}

void Z_FreeTags(int lowtag, int hightag)
{

   if (lowtag <= PU_FREE)
      lowtag = PU_FREE+1;

   if (hightag > PU_CACHE)
      hightag = PU_CACHE;

   for (;lowtag <= hightag; lowtag++)
   {
      memblock_t *block, *end_block;
      block = blockbytag[lowtag];
      if (!block)
         continue;
      end_block = block->prev;
      while (1)
      {
         memblock_t *next = block->next;
         (Z_Free)((uint8_t*) block + HEADER_SIZE);
         if (block == end_block)
            break;
         block = next;               // Advance to next block
      }
   }
}

/*
========================
=
= Z_ChangeTag
=
========================
*/

void Z_ChangeTag(void *ptr, int tag)
{
   memblock_t *block = (memblock_t *)((uint8_t*) ptr - HEADER_SIZE);

   // proff - added sanity check, this can happen when an empty lump is locked
   if (!ptr)
      return;

   // proff - do nothing if tag doesn't differ
   if (tag == block->tag)
      return;

   if (block == block->next)
      blockbytag[block->tag] = NULL;
   else
      if (blockbytag[block->tag] == block)
         blockbytag[block->tag] = block->next;
   block->prev->next = block->next;
   block->next->prev = block->prev;

   if (!blockbytag[tag])
   {
      blockbytag[tag] = block;
      block->next = block->prev = block;
   }
   else
   {
      blockbytag[tag]->prev->next = block;
      block->prev = blockbytag[tag]->prev;
      block->next = blockbytag[tag];
      blockbytag[tag]->prev = block;
   }

   block->tag = tag;
}

void *Z_Realloc(void *ptr, size_t n, int tag, void **user)
{
   void *p = (Z_Malloc)(n, tag, user);
   if (ptr)
   {
      memblock_t *block = (memblock_t *)((uint8_t*) ptr - HEADER_SIZE);

      if (n <= block->size)
        memcpy(p, ptr, n);
      else
      {
        memcpy(p, ptr, block->size);
        memset((char*)p+block->size, 0, n - block->size);
      }

      (Z_Free)(ptr);
      if (user) // in case Z_Free nullified same user
         *user=p;
   }
   return p;
}

void *Z_Calloc(size_t n1, size_t n2, int tag, void **user)
{
   if (n1 *= n2)
      return memset((Z_Malloc)(n1, tag, user), 0, n1);
   return NULL;
}

char *Z_Strdup(const char *s, int tag, void **user)
{
   return strcpy((Z_Malloc)(strlen(s)+1, tag, user), s);
}

void Z_SetPurgeLimit(int size)
{
   /* Only memory-starved platforms apply
    * a purge limit */
#ifdef MEMORY_LOW
   if (size == memory_size)
      return;

   if (size < MIN_MEMORY_SIZE)
   {
      I_Error("Z_SetPurgeLimit: Attempted to set a purge limit of less than 8 MB");
      size = MIN_MEMORY_SIZE;
   }
   memory_size = size;
#endif
}
