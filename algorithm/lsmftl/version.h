#ifndef __VERSION_H__
#define __VERSION_H__
#include <stdlib.h>
#include <stdint.h>
#include <queue>
#include "compaction.h"
#include "../../include/settings.h"
#include "../../include/sem_lock.h"
#include "lsmtree.h"
//#define TOTALRUNIDX 31
#define MERGED_RUN_NUM 2

typedef struct version{
	uint8_t start_hand;
	uint8_t end_hand;
	uint8_t *key_version;//key->ridx
	int8_t valid_version_num;
	int8_t total_version_number;
	int8_t max_valid_version_num;
	uint32_t *version_invalidation_cnt;
	bool *version_early_invalidate;
	fdriver_lock_t version_lock;
	std::queue<uint32_t> *ridx_empty_queue;
	std::queue<uint32_t> *ridx_populate_queue;
	uint32_t memory_usage_bit;
	int32_t poped_version_num;
}version;

version *version_init(uint8_t max_valid_version_num, uint8_t total_version_number, uint32_t LBA_num);
uint32_t version_get_empty_ridx(version *v);
void version_get_merge_target(version *v, uint32_t *ridx_set);
void version_unpopulate_run(version *v, uint32_t ridx);
void version_populate_run(version *v, uint32_t ridx);
void version_sanity_checker(version *v);
void version_free(version *v);
void version_coupling_lba_ridx(version *v, uint32_t lba, uint8_t ridx);
void version_reinit_early_invalidation(version *v, uint32_t ridx_num, uint32_t *ridx);
uint32_t version_get_max_invalidation_target(version *v, uint32_t *invalidated_num, uint32_t *avg_invalidated_num);
uint32_t version_update_for_trivial_move(version *v, uint32_t start_lba, uint32_t end_lba, uint32_t original_version, uint32_t target_version);
uint32_t version_get_early_invalidation_target(version *v);
void version_make_early_invalidation_enable_old(version *v);
static inline void version_enable_ealry_invalidation(version *v, uint32_t r_idx){
	v->version_early_invalidate[r_idx]=true;
}

static inline uint32_t version_map_lba(version *v, uint32_t lba){
	uint32_t res;
	fdriver_lock(&v->version_lock);
	res=v->key_version[lba];
	fdriver_unlock(&v->version_lock);
	return res;
}
static inline int version_to_run(version *v, int32_t a){
//	return 
	//return a-v->poped_version_num<0?a-v->poped_version_num+v->max_valid_version_num:a-v->poped_version_num;
	return a+v->poped_version_num >= 
		v->max_valid_version_num? a+v->poped_version_num-v->max_valid_version_num:
		a+v->poped_version_num;
}
static inline int version_compare(version *v, int32_t a, int32_t b){
	//a: recent version
	//b: noew version
	if(b > v->max_valid_version_num){
		EPRINT("not valid comparing", true);
	}
	//int a_=a-v->poped_version_num<0?a-v->poped_version_num+v->max_valid_version_num:a-v->max_valid_version_num;
	//int b_=b-v->poped_version_num<0?b-v->poped_version_num+v->max_valid_version_num:b-v->max_valid_version_num;

	int a_=a-v->poped_version_num<0?a-v->poped_version_num+v->max_valid_version_num:a-v->poped_version_num;
	int b_=b-v->poped_version_num<0?b-v->poped_version_num+v->max_valid_version_num:b-v->poped_version_num;
	return a_-b_;
}
static inline void version_poped_update(version *v){
	v->poped_version_num+=MERGED_RUN_NUM;
	v->poped_version_num%=v->max_valid_version_num;
}

static inline uint32_t version_level_idx_to_version(version *v, uint32_t lev_idx, uint32_t level_num){
	return level_num-lev_idx+v->max_valid_version_num-2;
}

static inline uint32_t version_to_level_idx(version *v, uint32_t version, uint32_t level_num){
	if(version<v->max_valid_version_num){
		return level_num-1;
	}
	else
		return level_num-version+v->max_valid_version_num-2;
}

static inline bool version_is_early_invalidate(version *v, uint32_t version){
	return !v->version_early_invalidate[version];
}

static inline void version_set_early_invalidation(version *v, uint32_t version){
	v->version_early_invalidate[version]=false;
}

#endif
