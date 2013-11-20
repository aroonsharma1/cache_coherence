#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include "cache.h"
#include "main.h"

/* cache configuration parameters */
static int cache_usize = DEFAULT_CACHE_SIZE;
static int cache_block_size = DEFAULT_CACHE_BLOCK_SIZE;
static int words_per_block = DEFAULT_CACHE_BLOCK_SIZE / WORD_SIZE;
static int cache_assoc = DEFAULT_CACHE_ASSOC;
static int cache_writeback = DEFAULT_CACHE_WRITEBACK;
static int cache_writealloc = DEFAULT_CACHE_WRITEALLOC;
static int num_core = DEFAULT_NUM_CORE;

/* cache model data structures */
/* max of 8 cores */
static cache mesi_cache[8];
static cache_stat mesi_cache_stat[8];

/************************************************************/
void set_cache_param(param, value)
  int param;
  int value;
{
  switch (param) {
  case NUM_CORE:
    num_core = value;
    break;
  case CACHE_PARAM_BLOCK_SIZE:
    cache_block_size = value;
    words_per_block = value / WORD_SIZE;
    break;
  case CACHE_PARAM_USIZE:
    cache_usize = value;
    break;
  case CACHE_PARAM_ASSOC:
    cache_assoc = value;
    break;
  default:
    printf("error set_cache_param: bad parameter value\n");
    exit(-1);
  }
}
/************************************************************/

/************************************************************/
void init_cache()
{
  /* initialize the cache, and cache statistics data structures */
	int i,j;
	for(i = 0; i < num_core; i++) {
		//initialize each core's cache
		mesi_cache[i].id = i;
		mesi_cache[i].size = cache_usize/4;
		mesi_cache[i].associativity = cache_assoc;
		mesi_cache[i].n_sets = cache_usize/cache_block_size/cache_assoc;
		mesi_cache[i].index_mask = (mesi_cache[i].n_sets-1) << LOG2(cache_block_size);
		mesi_cache[i].index_mask_offset = LOG2(cache_block_size);
		
		//allocate the array of cache line pointers for each cache
		mesi_cache[i].LRU_head = (Pcache_line *)malloc(sizeof(Pcache_line)*mesi_cache[i].n_sets);
		mesi_cache[i].LRU_tail = (Pcache_line *)malloc(sizeof(Pcache_line)*mesi_cache[i].n_sets);
		mesi_cache[i].set_contents = (int *)malloc(sizeof(int)*mesi_cache[i].n_sets);
		
		//allocate each cache line in the pointer array
		for(j = 0; j < mesi_cache[i].n_sets; j++) {
			mesi_cache[i].LRU_head[j] = NULL;
			mesi_cache[i].LRU_tail[j] = NULL;
			mesi_cache[i].set_contents[j] = 0;
		}
	}
}
/************************************************************/

/************************************************************/
void perform_access(addr, access_type, pid)
     unsigned addr, access_type, pid;
{
	int i;
  /* handle accesses to the mesi caches */
	unsigned int index = (addr & mesi_cache[pid].index_mask) >> mesi_cache[pid].index_mask_offset;
	unsigned tag = addr >> (LOG2(mesi_cache[pid].n_sets) + LOG2(cache_block_size));
	//printf("pid = %d, access_type = %d, index = %d, tag = %d\n", pid, access_type, index, tag);

	if(mesi_cache[pid].LRU_head[index] == NULL) {
		//compulsory miss (empty cache set)
		Pcache_line line = malloc(sizeof(cache_line));
		line->tag = tag;
		mesi_cache_stat[pid].accesses++;
		mesi_cache_stat[pid].misses++;
		mesi_cache_stat[pid].demand_fetches += cache_block_size/4;
		if(access_type == TRACE_LOAD) {
			//search other caches for a match
			if(remote_cache_match(index, tag, pid, access_type, mesi_cache) != 0) {
				//match found in remote cache?
				//if so change other cores to SHARED
				//change state of current_line to SHARED
				line->state = SHARED;
			}
			else {
				line->state = EXCLUSIVE;
			}
		}
		else if(access_type == TRACE_STORE) {
			//write miss from invalid
			line->state = MODIFIED;
		}
		insert(&mesi_cache[pid].LRU_head[index], &mesi_cache[pid].LRU_tail[index], line);
		mesi_cache[pid].set_contents[index]++;
	}
	else {
		//check for local cache hit
		mesi_cache_stat[pid].accesses++;
		Pcache_line current_line;
		for(current_line = mesi_cache[pid].LRU_head[index]; current_line != mesi_cache[pid].LRU_tail[index]->LRU_next; current_line = current_line->LRU_next) {
			if(current_line->tag == tag) {
				//hit in local cache?
				
				//read hit - do nothing
				
				if(access_type == TRACE_STORE) {
					//write hit
					remote_cache_match(index, tag, pid, access_type, mesi_cache);
					if(current_line->state == EXCLUSIVE) {
						current_line->state = MODIFIED;
					}
					else if(current_line->state == SHARED) {
						current_line->state = MODIFIED;
					}
				}
				Pcache_line line = current_line;
				delete(&mesi_cache[pid].LRU_head[index], &mesi_cache[pid].LRU_tail[index], current_line);
				insert(&mesi_cache[pid].LRU_head[index], &mesi_cache[pid].LRU_tail[index], line);
				return;
			}
		}
		mesi_cache_stat[pid].misses++;
		//cache miss check in remote caches
		if(remote_cache_match(index, tag, pid, access_type, mesi_cache) != 0) {
			Pcache_line remote_line = malloc(sizeof(cache_line));
			remote_line->tag = tag;
			if(access_type == TRACE_LOAD) {
				remote_line->state = SHARED;
			}
			else if(access_type == TRACE_STORE) {
				remote_line->state = MODIFIED;
			}
			
			if(mesi_cache[pid].set_contents[index] < cache_assoc) {
				//non-full cache set
				mesi_cache_stat[pid].demand_fetches += cache_block_size/4;
				insert(&mesi_cache[pid].LRU_head[index], &mesi_cache[pid].LRU_tail[index], remote_line);
				mesi_cache[pid].set_contents[index]++;
				return;
			}
			else {
				//full cache set - eviction
				mesi_cache_stat[pid].demand_fetches += cache_block_size/4;
				if(mesi_cache[pid].LRU_tail[index]->state == MODIFIED) {
					mesi_cache_stat[pid].copies_back += cache_block_size/4;
				}
				mesi_cache_stat[pid].replacements++;
				delete(&mesi_cache[pid].LRU_head[index], &mesi_cache[pid].LRU_tail[index], mesi_cache[pid].LRU_tail[index]);
				insert(&mesi_cache[pid].LRU_head[index], &mesi_cache[pid].LRU_tail[index], remote_line);
				return;
			}
			
		}

		else {
			//cache miss but main memory
			Pcache_line line = malloc(sizeof(cache_line));
			line->tag = tag;
			if(access_type == TRACE_LOAD) {
				line->state = EXCLUSIVE;
			}
			else if(access_type == TRACE_STORE) {
				line->state = MODIFIED;
			}
			if(mesi_cache[pid].set_contents[index] < cache_assoc) {
				//non-full cache set
				mesi_cache_stat[pid].demand_fetches += cache_block_size/4;
				insert(&mesi_cache[pid].LRU_head[index], &mesi_cache[pid].LRU_tail[index], line);
				mesi_cache[pid].set_contents[index]++;
			}
			else {
				//full cache set - eviction necessary
				mesi_cache_stat[pid].demand_fetches += cache_block_size/4;
				if(mesi_cache[pid].LRU_tail[index]->state == MODIFIED) {
					mesi_cache_stat[pid].copies_back += cache_block_size/4;
				}
				mesi_cache_stat[pid].replacements++;
				delete(&mesi_cache[pid].LRU_head[index], &mesi_cache[pid].LRU_tail[index], mesi_cache[pid].LRU_tail[index]);
				insert(&mesi_cache[pid].LRU_head[index], &mesi_cache[pid].LRU_tail[index], line);
			}
		}
	}
}
/************************************************************/

/************************************************************/
void flush()
{
  /* flush the mesi caches */
	int i, j;
	for(i = 0; i < num_core; i++) {
		for(j = 0; j < mesi_cache[i].n_sets; j++) {
			Pcache_line current_line;
			if(mesi_cache[i].LRU_head[j] != NULL) {
				for(current_line = mesi_cache[i].LRU_head[j]; current_line != mesi_cache[i].LRU_tail[j]->LRU_next; current_line = current_line->LRU_next) {
					if(current_line != NULL && current_line->state == MODIFIED) {
						mesi_cache_stat[i].copies_back += cache_block_size/4;
					}
				}
			}
		}
	}
}
/************************************************************/

/************************************************************/
void delete(head, tail, item)
  Pcache_line *head, *tail;
  Pcache_line item;
{
  if (item->LRU_prev) {
    item->LRU_prev->LRU_next = item->LRU_next;
  } else {
    /* item at head */
    *head = item->LRU_next;
  }

  if (item->LRU_next) {
    item->LRU_next->LRU_prev = item->LRU_prev;
  } else {
    /* item at tail */
    *tail = item->LRU_prev;
  }
}
/************************************************************/

/************************************************************/
/* inserts at the head of the list */
void insert(head, tail, item)
  Pcache_line *head, *tail;
  Pcache_line item;
{
  item->LRU_next = *head;
  item->LRU_prev = (Pcache_line)NULL;

  if (item->LRU_next)
    item->LRU_next->LRU_prev = item;
  else
    *tail = item;

  *head = item;
}
/************************************************************/

/************************************************************/
void dump_settings()
{
  printf("Cache Settings:\n");
  printf("\tSize: \t%d\n", cache_usize);
  printf("\tAssociativity: \t%d\n", cache_assoc);
  printf("\tBlock size: \t%d\n", cache_block_size);
}
/************************************************************/

/************************************************************/
void print_stats()
{
  int i;
  int demand_fetches = 0;
  int copies_back = 0;
  int broadcasts = 0;

  printf("*** CACHE STATISTICS ***\n");

  for (i = 0; i < num_core; i++) {
    printf("  CORE %d\n", i);
    printf("  accesses:  %d\n", mesi_cache_stat[i].accesses);
    printf("  misses:    %d\n", mesi_cache_stat[i].misses);
    printf("  miss rate: %f (%f)\n", 
	   (float)mesi_cache_stat[i].misses / (float)mesi_cache_stat[i].accesses,
	   1.0 - (float)mesi_cache_stat[i].misses / (float)mesi_cache_stat[i].accesses);
    printf("  replace:   %d\n", mesi_cache_stat[i].replacements);
  }

  printf("\n");
  printf("  TRAFFIC\n");
  for (i = 0; i < num_core; i++) {
    demand_fetches += mesi_cache_stat[i].demand_fetches;
    copies_back += mesi_cache_stat[i].copies_back;
    broadcasts += mesi_cache_stat[i].broadcasts;
  }
  printf("  demand fetch (words): %d\n", demand_fetches);
  /* number of broadcasts */
  printf("  broadcasts:           %d\n", broadcasts);
  printf("  copies back (words):  %d\n", copies_back);
}
/************************************************************/

/************************************************************/
//is there a matching cache block in a remote cache?
//update shared blocks' states if so
int remote_cache_match(unsigned index, unsigned tag, unsigned pid, unsigned access_type, Pcache mesi_cache) {
	int i, num_matching_cores = 0;
	mesi_cache_stat[pid].broadcasts++;
	for(i = 0; i < num_core; i++) {
		if(i != pid) {
			if(mesi_cache[i].LRU_head[index] != NULL) {
				Pcache_line current_line;
				for(current_line = mesi_cache[i].LRU_head[index]; current_line != mesi_cache[i].LRU_tail[index]->LRU_next; current_line = current_line->LRU_next) {
					if(current_line->tag == tag) {
						if(access_type == TRACE_LOAD) {
							current_line->state = SHARED;
							num_matching_cores++;
						}
						else if(access_type == TRACE_STORE) {
							delete(&mesi_cache[i].LRU_head[index], &mesi_cache[i].LRU_tail[index], current_line);
							mesi_cache[i].set_contents[index]--;
							num_matching_cores++;
							break;
						}
					}
				}
			}
		}
	}
	//no match found if this is 0
	return num_matching_cores;
}
/************************************************************/