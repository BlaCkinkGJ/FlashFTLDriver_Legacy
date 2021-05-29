#include "compaction.h"
#include "version.h"
#include "lsmtree.h"
#include "page_manager.h"
#include "io.h"
#include <queue>
#include <list>
#include <stdlib.h>
#include <map>
extern compaction_master *_cm;
extern lsmtree LSM;
extern uint32_t debug_lba;
void *merge_end_req(algo_req *);
void read_map_param_init(read_issue_arg *read_arg, map_range *mr){
	inter_read_alreq_param *param;
	uint32_t param_idx=0;
	for(int i=read_arg->from; i<=read_arg->to; i++){
		//param=compaction_get_read_param(_cm);
		param=(inter_read_alreq_param*)calloc(1, sizeof(inter_read_alreq_param));
		param->map_target=&mr[i];
		mr[i].data=NULL;
		param->data=inf_get_valueset(NULL, FS_MALLOC_R, PAGESIZE);
		fdriver_lock_init(&param->done_lock, 0);
		read_arg->param[param_idx++]=param;
	}
}

bool read_map_done_check(inter_read_alreq_param *param, bool check_page_sst){
	if(check_page_sst){
		param->map_target->data=param->data->value;
	}
	fdriver_lock(&param->done_lock);
	return true;
}

static bool map_done(inter_read_alreq_param *param){
	param->map_target->data=NULL;
	inf_free_valueset(param->data, FS_MALLOC_R);
	fdriver_destroy(&param->done_lock);
	invalidate_map_ppa(LSM.pm->bm, param->map_target->ppa, true);
	free(param);
	//compaction_free_read_param(_cm, param);
	return true;
}

static inline uint32_t coherence_sst_kp_pair_num(run *r, uint32_t start_idx, uint32_t *end_idx, uint32_t *map_num){
	uint32_t cnt=0;
	uint32_t now_map_num=0;
	sst_file *prev_sptr=NULL;
	sst_file *sptr;
	uint32_t iter_start_idx=start_idx;
	for(; iter_start_idx<r->now_sst_num; iter_start_idx++){
		sptr=&r->sst_set[iter_start_idx];
		if(!prev_sptr){
			prev_sptr=sptr;
			prev_sptr=&r->sst_set[iter_start_idx];
			now_map_num+=prev_sptr->map_num;
			continue;
		}
		if(sst_physical_range_overlap(prev_sptr, sptr)){
			prev_sptr=sptr;
			prev_sptr=&r->sst_set[iter_start_idx];
			now_map_num+=prev_sptr->map_num;
			cnt++;
		}
		else break;
	}

	*end_idx=start_idx+cnt;
	*map_num=now_map_num;

	return ((*map_num)*L2PGAP+(*map_num)*KP_IN_PAGE);
}

static map_range * make_mr_set(sst_file *set, uint32_t start_idx, uint32_t end_idx, uint32_t map_num){
	if(!map_num) return NULL;
	map_range *mr=(map_range*)calloc(map_num,sizeof(map_range));
	map_range *mptr;
	uint32_t idx=0;
	uint32_t mr_idx=0;
	for(uint32_t i=start_idx; i<=end_idx; i++){
		for_each_map_range(&set[i], mptr, idx){
			mr[mr_idx++]=*mptr;
		}
	}
	return mr;
}

static void  bulk_invalidation(run *r, uint32_t* border_idx, uint32_t border_lba){
	uint32_t i=0;

	for(i=(*border_idx); i<r->now_sst_num; i++){
		sst_file *sptr=&r->sst_set[i];
		if(sptr->end_lba<=border_lba){
			for(uint32_t j=sptr->file_addr.piece_ppa; j<sptr->end_ppa * L2PGAP; j++){
				invalidate_piece_ppa(LSM.pm->bm, j, false);
			}
			/* invalidate in map_done
			for(uint32_t j=sptr->end_ppa-sptr->map_num; j<sptr->end_ppa; j++){
				invalidate_map_ppa(LSM.pm->bm, j, true);
			}*/
		}
		else{
			break;
		}
	}
	(*border_idx)=i;
}

static bool tiering_invalidation_function(level *des, uint32_t stream_id, uint32_t version,
		key_ptr_pair kp, bool overlap){
	if(overlap){
		invalidate_piece_ppa(LSM.pm->bm, kp.piece_ppa, true);
		return false;
	}
	else{
		uint32_t a, b;
		a=version_map_lba(LSM.last_run_version, kp.lba);
		b=version;
		if(des->idx!=0 && !version_belong_level(LSM.last_run_version, a, des->idx-1)){
			if(version_compare(LSM.last_run_version, a, b) > 0){
				if(version_is_early_invalidate(LSM.last_run_version, b)){
					invalidate_piece_ppa(LSM.pm->bm, kp.piece_ppa, false);
				}
				else{
					invalidate_piece_ppa(LSM.pm->bm, kp.piece_ppa, true);
				}
				return false;
			}
		}
		return true;
	}
}


typedef struct mr_free_set{
	map_range *mr_set;
	run *r;
	uint32_t start_idx;
	uint32_t end_idx;
	uint32_t map_num;
}mr_free_set;

void map_range_postprocessing(std::list<mr_free_set>* mr_list,  uint32_t bound_lba, bool last){
	std::list<mr_free_set>::iterator mr_iter=mr_list->begin();
	for(;mr_iter!=mr_list->end(); ){
		mr_free_set now=*mr_iter;
		if(last || now.mr_set[now.map_num].end_lba <= bound_lba){
			for(uint32_t i=now.start_idx; i<=now.end_idx; i++){
				lsmtree_gc_unavailable_unset(&LSM, &now.r->sst_set[i], UINT32_MAX);
			}
			free(now.mr_set);
			mr_list->erase(mr_iter++);
		}
		else{
			break;
		}
	}
}


level* compaction_merge(compaction_master *cm, level *des, uint32_t *idx_set){
	_cm=cm;

	version_get_merge_target(LSM.last_run_version, idx_set, des->idx);
	run *new_run=run_init(des->max_sst_num/des->max_run_num, UINT32_MAX, 0);

	LSM.now_merging_run[0]=idx_set[0];
	LSM.now_merging_run[1]=idx_set[1];

	run *older=&des->array[idx_set[0]];
	run	*newer=&des->array[idx_set[1]];


	for(uint32_t i=0; i<MERGED_RUN_NUM; i++){
		version_unpopulate(LSM.last_run_version, idx_set[i], des->idx);
	}
	version_reinit_early_invalidation(LSM.last_run_version, MERGED_RUN_NUM, idx_set);

	read_issue_arg read_arg1={0,}, read_arg2={0,};
	read_issue_arg read_arg1_prev={0,}, read_arg2_prev={0,};
	read_arg_container thread_arg;
	thread_arg.end_req=merge_end_req;
	thread_arg.arg_set=(read_issue_arg**)malloc(sizeof(read_issue_arg*)*MERGED_RUN_NUM);
	thread_arg.arg_set[0]=&read_arg1;
	thread_arg.arg_set[1]=&read_arg2;
	thread_arg.set_num=MERGED_RUN_NUM;

	uint32_t newer_sst_idx=0, newer_sst_idx_end;
	uint32_t older_sst_idx=0, older_sst_idx_end;
	uint32_t now_newer_map_num=0, now_older_map_num=0;
	uint32_t newer_borderline=0;
	uint32_t older_borderline=0;
	uint32_t border_lba;

	uint32_t target_ridx=version_get_empty_version(LSM.last_run_version, des->idx);
	sst_pf_out_stream *os_set[MERGED_RUN_NUM]={0,};

	sst_bf_out_stream *bos=NULL;
	sst_bf_in_stream *bis=NULL;

	bool init=true;
	uint32_t max_target_piece_num;
	std::queue<key_ptr_pair> *kpq=new std::queue<key_ptr_pair>();
	std::list<mr_free_set> *new_range_set=new std::list<mr_free_set>();
	std::list<mr_free_set> *old_range_set=new std::list<mr_free_set>();
	while(!(older_sst_idx==older->now_sst_num && 
				newer_sst_idx==newer->now_sst_num)){
		now_newer_map_num=now_older_map_num=0;
		max_target_piece_num=0;
		max_target_piece_num+=
			newer_sst_idx<newer->now_sst_num?coherence_sst_kp_pair_num(newer,newer_sst_idx, &newer_sst_idx_end, &now_newer_map_num):0;
		max_target_piece_num+=
			older_sst_idx<older->now_sst_num?coherence_sst_kp_pair_num(older,older_sst_idx, &older_sst_idx_end, &now_older_map_num):0;

		if(bis){
			max_target_piece_num+=(bis->map_data->size()+1+1)*L2PGAP; // buffered + additional mapping
		}

		if(bos){
			max_target_piece_num+=bos->kv_wrapper_q->size()+L2PGAP;
		}

		uint32_t needed_seg_num=(max_target_piece_num/L2PGAP+1)/_PPS+1+1; //1 for front fragment, 1 for behid fragment
		if(page_manager_get_total_remain_page(LSM.pm, false, false) < needed_seg_num*_PPS){
	//		__do_gc(LSM.pm, false, max_target_piece_num/L2PGAP+(max_target_piece_num%L2PGAP?1:0));
			__do_gc(LSM.pm, false, needed_seg_num*_PPS);

			/*find */
			if(newer->sst_set[newer_sst_idx].trimed_sst_file){
				newer_sst_idx++;
			}
			if(older->sst_set[older_sst_idx].trimed_sst_file){
				older_sst_idx++;
			}
			continue;
		}

		map_range *newer_mr=make_mr_set(newer->sst_set, newer_sst_idx, newer_sst_idx_end, now_newer_map_num);
		map_range *older_mr=make_mr_set(older->sst_set, older_sst_idx, older_sst_idx_end, now_older_map_num);


		if(newer_mr){
			mr_free_set temp_mr_free_set={newer_mr, newer, newer_sst_idx, newer_sst_idx_end, now_newer_map_num-1};
			for(uint32_t i=newer_sst_idx; i<=newer_sst_idx_end; i++){
				lsmtree_gc_unavailable_set(&LSM, &newer->sst_set[i], UINT32_MAX);
			}
			new_range_set->push_back(temp_mr_free_set);
		}

		if(older_mr){
			mr_free_set temp_mr_free_set={older_mr, older, older_sst_idx, older_sst_idx_end, now_older_map_num-1};
			for(uint32_t i=older_sst_idx; i<=older_sst_idx_end; i++){
				lsmtree_gc_unavailable_set(&LSM, &older->sst_set[i], UINT32_MAX);
			}
			old_range_set->push_back(temp_mr_free_set);
		}

		bool last_round_check=((newer_sst_idx_end+1==newer->now_sst_num) && (older_sst_idx_end+1==older->now_sst_num));
		uint32_t total_map_num=now_newer_map_num+now_older_map_num;
		uint32_t read_done=0;
		uint32_t older_prev=0, newer_prev=0;
		read_arg1={0,}; read_arg2={0,};
		while(read_done!=total_map_num){
	/*		if(LSM.global_debug_flag){
				printf("merge cnt:%u round info - o_idx:%u n_idx%u read_done:%u\n", LSM.monitor.compaction_cnt[des->idx+1],older_sst_idx, newer_sst_idx, read_done);
			}*/
			uint32_t shard=LOWQDEPTH/(1+1);

			if(newer_mr){
				read_arg1.from=newer_prev;
				read_arg1.to=MIN(newer_prev+shard, now_newer_map_num-1);
				if(TARGETREADNUM(read_arg1)){
					read_map_param_init(&read_arg1, newer_mr);
				}
			}
			else{
				read_arg1.from=1;
				read_arg1.to=0;
			}
			
			if(older_mr){
				read_arg2.from=older_prev;
				read_arg2.to=MIN(older_prev+shard, now_older_map_num-1);
				if(TARGETREADNUM(read_arg2)){
					read_map_param_init(&read_arg2, older_mr);
				}
			}
			else{
				read_arg2.from=1;
				read_arg2.to=0;
			}

//			printf("read_arg1:%u~%u, read_arg2:%u~%u\n", read_arg1.from, read_arg1.to, read_arg2.from, read_arg2.to);

			//pos setting
			if(init){
				init=false;
				os_set[0]=sst_pos_init_mr(&newer_mr[read_arg1.from], read_arg1.param,
						TARGETREADNUM(read_arg1), 
						idx_set[1],
						read_map_done_check, map_done);
				os_set[1]=sst_pos_init_mr(&older_mr[read_arg2.from], read_arg2.param,
						TARGETREADNUM(read_arg2), 
						idx_set[0],
						read_map_done_check, map_done);
			}
			else{
				if(newer_mr){
					sst_pos_add_mr(os_set[0], &newer_mr[read_arg1.from], read_arg1.param,
							TARGETREADNUM(read_arg1));
				}
				if(older_mr){
					sst_pos_add_mr(os_set[1], &older_mr[read_arg2.from], read_arg2.param,
							TARGETREADNUM(read_arg2));
				}
			}

			thpool_add_work(cm->issue_worker, read_sst_job, (void*)&thread_arg);//read work
			
			if(newer_mr && older_mr){
				border_lba=MIN(newer_mr[read_arg1.to].end_lba, 
					older_mr[read_arg2.to].end_lba);
			}
			else{
				border_lba=(newer_mr?newer_mr[read_arg1.to].end_lba:
						older_mr[read_arg2.to].end_lba);
			}

		/*
			if((newer_mr && newer->sst_set[newer_sst_idx].map_num==1 && !older_mr) || 
					(older_mr && older->sst_set[older_sst_idx].map_num==1 && !newer_mr)){
				EPRINT("debug point", false);
			}*/

			//sorting
			LSM.monitor.merge_valid_entry_cnt+=stream_sorting(NULL, MERGED_RUN_NUM, os_set, NULL, kpq, 
				last_round_check,
				border_lba,/*limit*/
				target_ridx, 
				true,
				tiering_invalidation_function);

			read_done+=TARGETREADNUM(read_arg1)+TARGETREADNUM(read_arg2);
			if(newer_mr){
				newer_prev=read_arg1.to+1;
			}
			if(older_mr){
				older_prev=read_arg2.to+1;
			}
		}

		if(bos==NULL){
			bos=sst_bos_init(read_map_done_check, true);
		}
		if(bis==NULL){
			bis=tiering_new_bis(des->idx);
		}

		uint32_t entry_num=issue_read_kv_for_bos_sorted_set(bos, kpq, 
				true, idx_set[1], idx_set[0], last_round_check);
		border_lba=issue_write_kv_for_bis(&bis, bos, new_run, entry_num, 
				target_ridx, last_round_check);

		/*check end*/
		bulk_invalidation(newer, &newer_borderline, border_lba);
		bulk_invalidation(older, &older_borderline, border_lba);

		map_range_postprocessing(new_range_set, border_lba, last_round_check);
		map_range_postprocessing(old_range_set, border_lba, last_round_check);

		newer_sst_idx=newer_sst_idx_end+1;
		older_sst_idx=older_sst_idx_end+1;
		
		read_arg1_prev=read_arg1;
		read_arg2_prev=read_arg2;
	//	EPRINT("should delete before run", false);

	//	EPRINT("should delete before run", false);
		//return NULL;
	}

	sst_file *last_file;
	if((last_file=bis_to_sst_file(bis))){
		lsmtree_gc_unavailable_set(&LSM, last_file, UINT32_MAX);
		run_append_sstfile_move_originality(new_run, last_file);
		sst_free(last_file, LSM.pm);
	}

	if(bis->seg->used_page_num!=_PPS){
		if(LSM.pm->temp_data_segment){
			EPRINT("should be NULL", true);
		}
		LSM.pm->temp_data_segment=bis->seg;
	}

	sst_bis_free(bis);
	sst_bos_free(bos, _cm);

	LSM.monitor.merge_total_entry_cnt+=os_set[0]->total_poped_num+
		os_set[1]->total_poped_num;

	sst_pos_free(os_set[0]);
	sst_pos_free(os_set[1]);
	delete kpq;
	free(thread_arg.arg_set);

	level *res=level_init(des->max_sst_num, des->max_run_num, des->level_type, des->idx);
	//level_run_reinit(des, idx_set[1]);

	run *rptr; uint32_t ridx;
	for_each_run_max(des, rptr, ridx){
		if(ridx!=idx_set[0] && ridx!=idx_set[1]){
			if(rptr->now_sst_num){
				level_append_run_copy_move_originality(res, rptr, ridx);
			}
		}
	}

	level_update_run_at_move_originality(res, target_ridx, new_run, true);
	version_populate(LSM.last_run_version, target_ridx, des->idx);

	sst_file *temp_sptr;
	uint32_t temp_sidx;
	for_each_sst(new_run, temp_sptr, temp_sidx){
		lsmtree_gc_unavailable_unset(&LSM, temp_sptr, UINT32_MAX);
	}
	run_free(new_run);
//	level_print(res);
//	level_contents_print(res, true);
//	lsmtree_gc_unavailable_sanity_check(&LSM);
	version_poped_update(LSM.last_run_version);

	printf("merge %u,%u to %u\n", idx_set[0], idx_set[1], idx_set[0]);
	delete new_range_set;
	delete old_range_set;

	LSM.now_merging_run[0]=UINT32_MAX;
	LSM.now_merging_run[1]=UINT32_MAX;
	return res;
}

uint32_t update_read_arg_tiering(uint32_t read_done_flag, bool isfirst,sst_pf_out_stream **pos_set, 
		map_range **mr_set, read_issue_arg *read_arg_set, uint32_t stream_num, 
		level *src, uint32_t version){
	uint32_t remain_num=0;
	for(uint32_t i=0; i<stream_num; i++){
		if(read_done_flag & (1<<i)) continue;
		else remain_num++;
	}
	uint32_t start_version;
	if(isfirst){
		if(src){
			start_version=version_level_to_start_version(LSM.last_run_version, src->idx);
		}
		else{
			start_version=version;
		}
	}
	for(uint32_t i=0 ; i<stream_num; i++){
		if(read_done_flag & (1<<i)) continue;
		if(!isfirst && read_arg_set[i].to==read_arg_set[i].max_num-1){
			read_done_flag|=(1<<i);
			continue;
		}

		if(isfirst){
			read_arg_set[i].to=MIN(read_arg_set[i].from+COMPACTION_TAGS/remain_num, 
					read_arg_set[i].max_num-1);
			read_map_param_init(&read_arg_set[i], mr_set[i]);
			pos_set[i]=sst_pos_init_mr(&mr_set[i][read_arg_set[i].from], 
					read_arg_set[i].param, TARGETREADNUM(read_arg_set[i]),
					start_version+stream_num-1-i, //to set ridx_version
					read_map_done_check, map_done);
		}
		else{
			read_arg_set[i].from=read_arg_set[i].to+1;
			read_arg_set[i].to=MIN(read_arg_set[i].from+COMPACTION_TAGS/remain_num, 
					read_arg_set[i].max_num-1);
			read_map_param_init(&read_arg_set[i], mr_set[i]);
			sst_pos_add_mr(pos_set[i], &mr_set[i][read_arg_set[i].from], 
					read_arg_set[i].param, TARGETREADNUM(read_arg_set[i]));
		}
	}
	return read_done_flag;
}

run* tiering_trivial_move(level *src){
	run *src_rptr; uint32_t src_idx;
	for_each_run(src, src_rptr, src_idx){
		run *comp_rptr; uint32_t comp_idx=src_idx+1;
		for_each_run_at(src, comp_rptr, comp_idx){
			if((src_rptr->start_lba > comp_rptr->end_lba) ||
				(src_rptr->end_lba<comp_rptr->start_lba)){
				continue;
			}
			else{
				return NULL;
			}
		}
	}

	std::map<uint32_t, run*> temp_run;
	for_each_run(src, src_rptr, src_idx){
		temp_run.insert(std::pair<uint32_t, run*>(src_rptr->start_lba, src_rptr));
	}

	run *res=run_init(src->max_sst_num, UINT32_MAX, 0);
	std::map<uint32_t, run*>::iterator iter;
	for(iter=temp_run.begin(); iter!=temp_run.end(); iter++){
		run *rptr=iter->second;
		sst_file *sptr; uint32_t sidx;
		for_each_sst(rptr, sptr, sidx){	
			run_append_sstfile_move_originality(res, sptr);
		}
	}
	return res;
}

level* compaction_TI2TI(compaction_master *cm, level *src, level *des, uint32_t target_version){
	_cm=cm;
	run *new_run=NULL;
	uint32_t stream_num=src->run_num;
	uint32_t target_run_idx=version_to_ridx(LSM.last_run_version, target_version, des->idx);
	if((new_run=tiering_trivial_move(src))){
		level *res=level_init(des->max_sst_num, des->max_run_num, des->level_type, des->idx);
		run *rptr; uint32_t ridx;
		for_each_run_max(des, rptr, ridx){
			if(rptr->now_sst_num){
				level_append_run_copy_move_originality(res, rptr, ridx);
			}
		}

		level_update_run_at_move_originality(res, target_run_idx, new_run, true);
		run_free(new_run);
		return res;
	}
	else{
		new_run=run_init(src->max_sst_num, UINT32_MAX, 0);
	}

	level *res=level_init(des->max_sst_num, des->max_run_num, des->level_type, des->idx);
	run *rptr; uint32_t ridx;
	for_each_run_max(src, rptr, ridx){
		sst_file *sptr; uint32_t sidx;
		for_each_sst(rptr, sptr, sidx){
			lsmtree_gc_unavailable_set(&LSM, sptr, UINT32_MAX);
		}
	}

	read_issue_arg *read_arg_set=(read_issue_arg*)calloc(stream_num, sizeof(read_issue_arg));
	read_arg_container thread_arg;
	thread_arg.end_req=merge_end_req;
	thread_arg.arg_set=(read_issue_arg**)calloc(stream_num, sizeof(read_issue_arg*));
	for(int32_t i=stream_num-1; i>=0; i--){
		thread_arg.arg_set[i]=&read_arg_set[i];
	}
	thread_arg.set_num=stream_num;

	map_range **mr_set=(map_range **)calloc(stream_num, sizeof(map_range*));
	/*make it reverse order for stream sorting*/
	for(int32_t i=stream_num-1, j=0; i>=0; i--, j++){
		uint32_t sst_file_num=src->array[i].now_sst_num;
		uint32_t map_num=0;
		for(uint32_t j=0; j<sst_file_num; j++){
			map_num+=src->array[i].sst_set[j].map_num;
		}
		read_arg_set[j].max_num=map_num;
		mr_set[j]=make_mr_set(src->array[i].sst_set, 0, src->array[i].now_sst_num-1, map_num);
	}

	std::queue<key_ptr_pair> *kpq=new std::queue<key_ptr_pair>();
	uint32_t sorting_done=0;
	uint32_t read_done=0;
	sst_pf_out_stream **pos_set=(sst_pf_out_stream **)calloc(stream_num, sizeof(sst_pf_out_stream*));
	sst_bf_out_stream *bos=NULL;
	sst_bf_in_stream *bis=NULL;
	bool isfirst=true;	
	while(!(sorting_done==((1<<stream_num)-1) && read_done==((1<<stream_num)-1))){
		uint32_t border_lba=UINT32_MAX;
		if(!isfirst && des->idx==LSM.param.LEVELN-1){
			LSM.global_debug_flag=true;
		}
		read_done=update_read_arg_tiering(read_done, isfirst, pos_set, mr_set,
				read_arg_set, stream_num, src, UINT32_MAX);
		bool last_round=(read_done==(1<<stream_num)-1);
		if(!last_round){
			thpool_add_work(cm->issue_worker, read_sst_job, (void*)&thread_arg);
		}

		uint32_t sorted_entry_num=stream_sorting(res, stream_num, pos_set, NULL, kpq,
				last_round,
				border_lba,/*limit*/
				target_version, 
				true,
				tiering_invalidation_function);

		if(bos==NULL){
			bos=sst_bos_init(read_map_done_check, true);
		}
		if(bis==NULL){
			bis=tiering_new_bis(des->idx);	
		}
		if(last_round && sorted_entry_num==0){
			uint32_t read_num=issue_read_kv_for_bos_sorted_set(bos, kpq, false, UINT32_MAX, UINT32_MAX, last_round);
			border_lba=issue_write_kv_for_bis(&bis, bos, new_run, read_num, target_version, last_round);
		}
		else{
			for(uint32_t moved_num=0; moved_num<sorted_entry_num; ){
				uint32_t read_num=issue_read_kv_for_bos_sorted_set(bos, kpq, false, UINT32_MAX, UINT32_MAX, last_round);
				border_lba=issue_write_kv_for_bis(&bis, bos, new_run, read_num, target_version, last_round);
				moved_num+=read_num;
			}
		}

		for(uint32_t i=0; i<stream_num; i++){
			if(!(read_done & (1<<i))) continue;
			if((sorting_done & (1<<i))) continue;
			if(sst_pos_is_empty(pos_set[i])){
				sorting_done |=(1<<i);
			}
		}
		isfirst=false;
	}


	sst_file *last_file;
	if((last_file=bis_to_sst_file(bis))){
		lsmtree_gc_unavailable_set(&LSM, last_file, UINT32_MAX);
		run_append_sstfile_move_originality(new_run, last_file);
		sst_free(last_file, LSM.pm);
	}

	for(uint32_t i=0; i<stream_num; i++){
		sst_pos_free(pos_set[i]);
	}

	//level_run_reinit(des, idx_set[1]);

	for_each_run_max(des, rptr, ridx){
		if(rptr->now_sst_num){
			level_append_run_copy_move_originality(res, rptr, ridx);
		}
	}

	level_update_run_at_move_originality(res, target_run_idx, new_run, true);

	for_each_run_max(src, rptr, ridx){
		sst_file *sptr; uint32_t sidx;
		for_each_sst(rptr, sptr, sidx){
			lsmtree_gc_unavailable_unset(&LSM, sptr, UINT32_MAX);
		}
	}

	run_free(new_run);
	sst_bis_free(bis);
	sst_bos_free(bos, _cm);

	for(uint32_t i=0; i<stream_num; i++){
		free(mr_set[i]);
	}

	delete kpq;
	free(pos_set);
	free(mr_set);
	free(thread_arg.arg_set);
	free(read_arg_set);
	level_print(res);
	return res;
}

level *compaction_TW_convert_LW(compaction_master *cm, level *src){
	_cm=cm;
	run *new_run=NULL;
	uint32_t stream_num=src->run_num;
	level *res=level_init(src->now_sst_num, 1, LEVELING_WISCKEY, src->idx);
	if((new_run=tiering_trivial_move(src))){

		sst_file *sptr; uint32_t sidx;
		for_each_sst(new_run, sptr, sidx){
			level_append_sstfile(res, sptr, true);
		}
		return res;
	}

	read_issue_arg *read_arg_set=(read_issue_arg*)calloc(stream_num, sizeof(read_issue_arg));
	read_arg_container thread_arg;
	thread_arg.end_req=merge_end_req;
	thread_arg.arg_set=(read_issue_arg**)calloc(stream_num, sizeof(read_issue_arg*));
	for(int32_t i=stream_num-1; i>=0; i--){
		thread_arg.arg_set[i]=&read_arg_set[i];
	}
	thread_arg.set_num=stream_num;

	map_range **mr_set=(map_range **)calloc(stream_num, sizeof(map_range*));
	run *rptr; uint32_t ridx; uint32_t set_idx=0;
	/*make it reverse order for stream sorting*/
	for_each_run_reverse(src, rptr, ridx){
		mr_set[set_idx]=run_to_MR(rptr);
		read_arg_set[set_idx].max_num=rptr->now_sst_num;
		set_idx++;
	}

	sst_pf_out_stream **pos_set=(sst_pf_out_stream **)calloc(stream_num, sizeof(sst_pf_out_stream*));
	uint32_t read_done=0;
	uint32_t sorting_done=0;
	bool isfirst=true;
	read_helper_param temp_rhp;
	sst_pf_in_stream *pis=sst_pis_init(false, temp_rhp);
	uint32_t target_version=version_level_to_start_version(LSM.last_run_version, src->idx) 
		+ src->run_num-1;
	while(!(sorting_done==((1<<stream_num)-1) && read_done==((1<<stream_num)-1))){
		read_done=update_read_arg_tiering(read_done, isfirst, pos_set, mr_set,
				read_arg_set, stream_num, src, UINT32_MAX);
		bool last_round=(read_done==(1<<stream_num)-1);

		if(!last_round){
			thpool_add_work(cm->issue_worker, read_sst_job, (void*)&thread_arg);
		}
	
		stream_sorting(res, stream_num, pos_set, pis, NULL,
				last_round,
				UINT32_MAX,/*limit*/
				target_version, 
				true,
				tiering_invalidation_function);

		for(uint32_t i=0; i<stream_num; i++){
			if(!(read_done & (1<<i))) continue;
			if((sorting_done & (1<<i))) continue;
			if(sst_pos_is_empty(pos_set[i])){
				sorting_done |=(1<<i);
			}
		}
		isfirst=false;
	}

	thpool_wait(cm->issue_worker);

	for(uint32_t i=0; i<stream_num; i++){
		free(mr_set[i]);
		sst_pos_free(pos_set[i]);
	}

	sst_pis_free(pis);
	free(pos_set);

	free(mr_set);
	free(thread_arg.arg_set);
	free(read_arg_set);
	return res;
}

static uint32_t filter_invalidation(sst_pf_out_stream *pos, std::queue<key_ptr_pair> *kpq, 
		uint32_t now_version){
	uint32_t valid_num=0;
	while(!sst_pos_is_empty(pos)){
		key_ptr_pair target_pair=sst_pos_pick(pos);
		uint32_t recent_version=version_map_lba(LSM.last_run_version, target_pair.lba);
		if(now_version==recent_version || 
				version_compare(LSM.last_run_version, now_version, recent_version)>0){
			kpq->push(target_pair);
			valid_num++;
		}
		else{
			invalidate_piece_ppa(LSM.pm->bm, target_pair.piece_ppa, true);
		}
		sst_pos_pop(pos);
	}
	return valid_num;
}

run *compaction_reclaim_run(compaction_master *cm, run *target_rptr, uint32_t version){
	_cm=cm;

	read_issue_arg read_arg_set;
	read_arg_container thread_arg;
	thread_arg.end_req=comp_alreq_end_req;
	thread_arg.arg_set=(read_issue_arg**)malloc(sizeof(read_issue_arg));
	thread_arg.arg_set[0]=&read_arg_set;
	thread_arg.set_num=1;

	sst_pf_out_stream *pos=NULL;
	map_range *mr_set;
	mr_set=run_to_MR(target_rptr);
	uint32_t stream_num=1;
	uint32_t read_done=0;
	bool isfirst=true;
	bool last_round=false;
	std::queue<key_ptr_pair> *kpq=new std::queue<key_ptr_pair>();
	sst_bf_out_stream *bos=NULL;
	sst_bf_in_stream *bis=NULL;
	uint32_t border_lba=0;

	run *new_run=run_init(target_rptr->now_sst_num, UINT32_MAX, 0);

	while(read_done!=(1<<stream_num)-1){
		read_done=update_read_arg_tiering(read_done, isfirst, &pos, &mr_set, 
				&read_arg_set, stream_num, NULL, version);
		last_round=(read_done==(1<<stream_num)-1);
		if(!last_round){
			thpool_add_work(cm->issue_worker, read_sst_job, (void*)&thread_arg);
		}

		filter_invalidation(pos, kpq, version);

		if(bos==NULL){
			bos=sst_bos_init(read_map_done_check, true);
		}
		if(bis==NULL){
			bis=tiering_new_bis(LSM.param.LEVELN-1);	
		}

		uint32_t read_num=issue_read_kv_for_bos_sorted_set(bos, kpq, false, UINT32_MAX, UINT32_MAX, 
				last_round);
		border_lba=issue_write_kv_for_bis(&bis, bos, new_run, read_num, version, last_round);
		isfirst=false;
	}

	sst_file *last_file;
	if((last_file=bis_to_sst_file(bis))){
		lsmtree_gc_unavailable_set(&LSM, last_file, UINT32_MAX);
		run_append_sstfile_move_originality(new_run, last_file);
		sst_free(last_file, LSM.pm);
	}

	sst_bis_free(bis);
	sst_bos_free(bos, _cm);
	delete kpq;
	free(mr_set);
	free(thread_arg.arg_set);
	return new_run;
}

void *merge_end_req(algo_req *req){
	inter_read_alreq_param *r_param;
	key_value_wrapper *kv_wrapper;
	switch(req->type){
		case MAPPINGR:
			r_param=(inter_read_alreq_param*)req->param;
			fdriver_unlock(&r_param->done_lock);
			break;
		case COMPACTIONDATAR:
			kv_wrapper=(key_value_wrapper*)req->param;

			fdriver_unlock(&kv_wrapper->param->done_lock);
			break;

	}
	free(req);
	return NULL;
}
