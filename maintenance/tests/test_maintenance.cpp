#define main embedded_runner_main
#include "../experiments/tools/compare_policies.cpp"
#undef main
#include <cassert>
#include <future>
#include <cstdlib>
#include <sstream>

static void demand(bool condition,const char* reason){if(!condition)throw std::runtime_error(reason);}
static void io_checks(const std::string& base,const std::string& fault){
    DiskProfile profile;profile.base_mean_ms=0;profile.base_jitter_ms=0;
    DiskSimulator disk(base,2,profile,123);
    disk.write_shard(0,99,{1,2,3});
    if(!fault.empty()){
        setenv("HEALTHEC_IO_FAULT",fault.c_str(),1);bool failed=false;
        try{disk.write_shard(0,99,{4,5,6});}catch(...){failed=true;}
        unsetenv("HEALTHEC_IO_FAULT");
        demand(failed,"injected write/close failure was accepted");
        demand(disk.read_data(0,99)==std::vector<uint8_t>({1,2,3}),"failed write destroyed original");
        return;
    }
    for(int i=0;i<50;++i){
        disk.write_shard(0,99,{1,2,3});
        auto first=std::async(std::launch::async,[&]{disk.write_shard(0,99,{4,5,6});});
        auto second=std::async(std::launch::async,[&]{disk.migrate_shard(0,99,1);});
        first.get();second.get();
        auto data=disk.read_data(1,99);
        demand(data==std::vector<uint8_t>({1,2,3}) || data==std::vector<uint8_t>({4,5,6}),"concurrent move copied incomplete data");
    }
    std::filesystem::create_directory(std::filesystem::path(base)/"disk0/shard100.bin");
    bool failed=false;try{disk.read_shard(0,100,false);}catch(...){failed=true;}
    demand(failed,"existing unreadable shard was reported successful");
    ScoreManager scores;
    ReadScheduler reader(1,1,scores,[](DiskId,ShardId,bool)->ShardReadResult{throw std::runtime_error("read fault");});
    StripeLayout layout;layout.data_shards={0};layout.parity_shards={1};layout.disk_of={{0,0},{1,1}};
    auto read=std::async(std::launch::async,[&]{try{reader.read_stripe(0,layout,1);return false;}catch(...){return true;}});
    demand(read.wait_for(std::chrono::seconds(2))==std::future_status::ready && read.get(),"read failure hung scheduler");
    DiskProfile spike;spike.slow_mode=true;spike.slow_mean_ms=0;spike.slow_jitter_ms=100;
    spike.spike_prob=1;spike.spike_ms=10;disk.set_profile(0,spike);
    for(int i=0;i<1000;++i){
        demand(disk.sample_latency_ms(0)>=10,"negative normal cancelled spike");
        demand(deterministic_latency_ms(i,i,0,0,false,spike)>=10,"latent normal cancelled spike");
    }
}

static void migration_checks(){
    ScoreManager scores;scores.update_health(0,.9,0,0);scores.update_health(1,.1,0,0);
    scores.update_slowness(99,10,1);scores.update_death(99,10,1);
    const double slow=scores.get_slowness(99),death=scores.get_death(99);
    MigrationScheduler* pointer=nullptr;int calls=0;
    MigrationScheduler scheduler(scores,[&](DiskId,ShardId shard,DiskId){
        ++calls;if(calls==1){pointer->enqueue(shard,0,100);throw std::runtime_error("fault");}
    });pointer=&scheduler;scheduler.enqueue(99,0,90);scheduler.tick_once();
    demand(scores.get_slowness(99)==slow && scores.get_death(99)==death,"failed migration cleared score");
    scheduler.tick_once();demand(calls==2,"failure erased newer candidate generation");
    MigrationStrategyConfig strategy;strategy.kind=MigrationStrategyKind::WindowEvidence;strategy.evidence_windows=3;
    std::unordered_map<ShardId,EvidenceState> evidence;
    for(int w=0;w<3;++w)record_migration_evidence(evidence,99,w);
    demand(evidence_gate_passes(strategy,evidence,99,2),"fresh evidence rejected");
    demand(!evidence_gate_passes(strategy,evidence,99,3),"stale evidence accepted");
    demand(evidence_gate_passes(strategy,evidence,99,3,1),"next-window budgeted evidence rejected");
    RuntimeConfig cfg;cfg.scenario="dynamic_overlap_100d_4pct_hdd";cfg.num_reads=20000;cfg.num_stripes=500;
    cfg.health_ec_params.theta_S=.01;cfg.health_ec_params.theta_D=.001;
    auto spec=scenario_spec_for_name(cfg.scenario);auto layout=build_layout(cfg.num_stripes,spec);
    auto world=build_dynamic_world(cfg,layout);auto config=make_policy_config(PolicyKind::HealthEC,cfg,cfg.health_ec_params);
    config.migration_strategy=strategy;
    auto run=run_dynamic_policy(world,layout,config);
    demand(!run.migration_trace.empty(),"evidence fixture produced no migration");
    std::unordered_map<ShardId,int> last;
    for(const auto& row:run.migration_trace){
        auto it=last.find(row.shard_id);
        demand(it==last.end() || row.window_id-it->second>=3,"layout change reused old evidence");
        last[row.shard_id]=row.window_id;
    }
    config.migration_strategy.kind=MigrationStrategyKind::Hybrid;config.migration_strategy.top_k_per_window=1;
    auto hybrid=run_dynamic_policy(world,layout,config);std::map<int,int> per_window;
    demand(!hybrid.migration_trace.empty(),"hybrid fixture produced no migration");
    for(const auto& row:hybrid.migration_trace)demand(++per_window[row.window_id]<=1,"hybrid exceeded per-window budget");
    auto late=make_policy_config(PolicyKind::LateBinding,cfg,cfg.health_ec_params);
    auto observed=run_dynamic_policy(world,layout,late,1000);
    std::vector<std::vector<double>> sums(20,std::vector<double>(world.num_disks));
    std::vector<std::vector<long>> counts(20,std::vector<long>(world.num_disks));
    for(const auto& request:world.requests){
        auto add=[&](const LatentShard& shard){
            double latency=shard.latency_by_disk.empty()?shard.latency_ms:shard.latency_by_disk.at(shard.disk);
            sums[request.read_index/1000][shard.disk]+=latency;++counts[request.read_index/1000][shard.disk];
        };
        for(const auto& shard:request.data)add(shard);
        add(request.parity.front());
    }
    std::ostringstream output;auto old=std::cout.rdbuf(output.rdbuf());
    RuntimeConfig printed=cfg;printed.detector_bin_reads=1000;
    print_disk_trace(printed,world,{late},{observed});std::cout.rdbuf(old);
    std::istringstream lines(output.str());std::string line;std::getline(lines,line);
    while(std::getline(lines,line)){
        std::vector<std::string> fields;std::istringstream row(line);std::string field;
        while(std::getline(row,field,','))fields.push_back(field);
        int disk=std::stoi(fields.at(3)),bin=std::stoi(fields.at(4));
        demand(std::stol(fields.at(7))==counts[bin][disk],"issued-read observation denominator mismatch");
        if(counts[bin][disk])demand(std::abs(std::stod(fields.at(11))-sums[bin][disk]/counts[bin][disk])<1e-7,
                                   "complete issued-read mean mismatch");
    }
}

static void scale_profile_checks(){
    auto mild=profile_for_state(DiskState::MildSlow,.5);
    auto recovery=profile_for_state(DiskState::Recovery,1.5);
    auto close=[](double a,double b){return std::abs(a-b)<1e-12;};
    demand(close(mild.slow_mean_ms,16.5) && close(mild.slow_jitter_ms,5.75) &&
           close(mild.spike_prob,.005) && close(mild.spike_ms,75),"mild scaling formula mismatch");
    demand(close(recovery.slow_mean_ms,15.75) && close(recovery.slow_jitter_ms,5.75) &&
           close(recovery.spike_prob,.0075) && close(recovery.spike_ms,120),"recovery scaling formula mismatch");
    for(double scale:{.5,1.,1.5}){
        auto healthy=profile_for_state(DiskState::Healthy,scale);
        demand(close(healthy.base_mean_ms,9) && close(healthy.base_jitter_ms,3.5) && !healthy.slow_mode,
               "scaling changed healthy profile");
    }
}

static void config_checks(){
    scale_profile_checks();
    RuntimeConfig cfg;cfg.migration_strategy.kind=MigrationStrategyKind::Hybrid;
    bool rejected=false;try{validate_runtime_config(cfg);}catch(...){rejected=true;}
    demand(rejected,"static unsupported strategy accepted");
    cfg.scenario="dynamic_degradation";cfg.slowdown_scale=1e308;rejected=false;
    try{validate_runtime_config(cfg);}catch(...){rejected=true;}
    demand(rejected,"nonfinite derived profile accepted");
    cfg.slowdown_scale=1;cfg.migration_strategy.cooldown_windows=std::numeric_limits<int>::max();rejected=false;
    try{validate_runtime_config(cfg);}catch(...){rejected=true;}
    demand(rejected,"cooldown overflow accepted");
}

int main(int argc,char** argv){
    std::string fault=getenv("HEALTHEC_IO_FAULT")?getenv("HEALTHEC_IO_FAULT"):"";
    unsetenv("HEALTHEC_IO_FAULT");
    auto base=(std::filesystem::temp_directory_path()/("health_ec_fix_"+std::to_string(getpid()))).string();
    try{io_checks(base,fault);if(fault.empty()){migration_checks();config_checks();}}
    catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}
    std::filesystem::remove_all(base);return 0;
}
