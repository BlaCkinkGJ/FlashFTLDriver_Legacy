#include "../settings.h"
#include "../sem_lock.h"
#include "../settings.h"
#include "randomsequence.h"
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <random>

uint32_t *keymap;
static uint32_t my_seed;
static fdriver_lock_t data_check_lock;
uint32_t test_key=UINT32_MAX;

int str2int(const char* str, int len)
{
    int i;
    int ret = 0;
    for(i = 0; i < len; ++i)
    {
        ret = ret * 10 + (str[i] - '0');
    }
    return ret;
}

void __checking_data_init(){
	keymap=(uint32_t *)malloc(sizeof(uint32_t)*RANGE);
	fdriver_mutex_init(&data_check_lock);
}

void __checking_data_make(uint32_t key, char *data){
	static bool isstart=false;
	if(!isstart){
		isstart=true;
		printf("data checking start!!!\n");
	}
	keymap[key]=my_seed;
	fdriver_lock(&data_check_lock);
	if(key==test_key){
		printf("target populate - seed:%u\n debuging code\n",keymap[key]);
	}
	RandomSequenceOfUnique rng(my_seed, my_seed);
	for(uint32_t i=0; i<LPAGESIZE/sizeof(uint32_t); i++){
		uint32_t t=rng.next();
		if(key==test_key && i<10){
			printf("%u ", t);
		}
		memcpy(&data[i*sizeof(uint32_t)], &t, sizeof(uint32_t));
	}
	if(key==test_key){
		printf("\ndata print\n");
	}
	my_seed++;
	fdriver_unlock(&data_check_lock);
}
#ifdef KVSSD
void __checking_data_make_key(KEYT _key, char *data){
	uint32_t key=str2int(_key.key, _key.len);
	fdriver_lock(&data_check_lock);
	keymap[key]=my_seed;
	if(key==test_key){
		printf("target populate - seed:%u\n debuging code\n",keymap[key]);
	}
	RandomSequenceOfUnique rng(my_seed, my_seed);
	for(uint32_t i=0; i<LPAGESIZE/sizeof(uint32_t); i++){
		uint32_t t=rng.next();
		memcpy(&data[i*sizeof(uint32_t)], &t, sizeof(uint32_t));
		if(key==test_key && i<10){
			printf("%u ", t);
		}
	}
	if(key==test_key){
		printf("\ndata print\n");
	}
	my_seed++;
	fdriver_unlock(&data_check_lock);
}

bool __checking_data_check_key(KEYT _key, char *data){
	uint32_t key=str2int(_key.key, _key.len);
	fdriver_lock(&data_check_lock);
	uint32_t test_seed=keymap[key];
	uint32_t t;
	static int miss_cnt=0;
	RandomSequenceOfUnique rng(test_seed, test_seed);
	if(key==test_key){
		printf("target check - seed: %u debugin code\n", keymap[key]);
	}
	for(uint32_t i=0; i<LPAGESIZE/sizeof(uint32_t); i++){
		memcpy(&t, &data[i*sizeof(uint32_t)], sizeof(uint32_t));
		uint32_t tt=rng.next();
		if(key==test_key && i<10){
			printf("%u %u (read : org)\n", t, tt);
		}
		if(tt != t){
			printf("data miss %u!!!!\n", miss_cnt++);
		}
	}
	fdriver_unlock(&data_check_lock);
	return true;
}
#endif
bool __checking_data_check(uint32_t key, char *data, bool *iszero){
	uint32_t test_seed=keymap[key];
	fdriver_lock(&data_check_lock);
	uint32_t t;
	RandomSequenceOfUnique rng(test_seed, test_seed);
	static int cnt=0;
	if(key==test_key){
		printf("target check - seed: %u debugin code\n", keymap[key]);
	}
	bool is_no_error=true;
	for(uint32_t i=0; i<LPAGESIZE/sizeof(uint32_t); i++){
		memcpy(&t, &data[i*sizeof(uint32_t)], sizeof(uint32_t));
		uint32_t tt=rng.next();
		if(key==test_key && i<10){
			printf("%u %u (read : org)\n", t, tt);
		}
		if(tt != t){
				
	//		printf("lba:%u data miss: %u!!!!\n", key, cnt++);
			fdriver_unlock(&data_check_lock);
		//	printf("error at %u read:%x org:%x\n", i, t, tt);
			if(t==0){
				*iszero=true;
			}
			is_no_error= false;
			//abort();
		}
	}
	fdriver_unlock(&data_check_lock);

	RandomSequenceOfUnique rng2(test_seed, test_seed);
	if(is_no_error==false){
		printf("printing error data (writtend data:read data)\n");
		for(uint32_t i=0; i<LPAGESIZE/sizeof(uint32_t); i++){
			uint32_t org_value=rng2.next();
			memcpy(&t, &data[i*sizeof(uint32_t)], sizeof(uint32_t));
			printf("%x:%x\t", org_value, t);
			if((i+1)%4==0){
				printf("\n");
			}
		}
	}
	return is_no_error;
}

void __checking_data_free(){
	free(keymap);
	fdriver_destroy(&data_check_lock);
}
