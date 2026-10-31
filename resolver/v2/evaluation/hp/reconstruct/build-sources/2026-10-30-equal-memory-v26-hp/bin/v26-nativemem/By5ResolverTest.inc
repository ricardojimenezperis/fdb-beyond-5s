#define BY5_TEMP_BTREE 1
#include "By5Workload.h"
#include "FictitiousHistory.h"
#include <cstring>
#include <cstdlib>
#include <cerrno>
#include <iomanip>
#ifdef __linux__
#include <sys/resource.h>
#endif
namespace {
uint64_t by5Env(const char* name,uint64_t fallback){
 const char*v=std::getenv(name);if(!v)return fallback;
 by5::require(*v && *v!='-',name);char* end=nullptr;errno=0;auto n=std::strtoull(v,&end,10);by5::require(!errno && !*end,name);return n;
}
struct By5Counts{uint64_t accepted=0,conflicts=0,window=0,memory=0,target=0,divergent=0,readWidth=0,writeWidth=0,hash=14695981039346656037ULL;};
}
void by5ResolverTest(){
 const char* path=std::getenv("BY5_TRACE");by5::require(path && *path,"BY5_TRACE required");
 std::ifstream trace(path,std::ios::binary);auto cfg=by5::readHeader(trace);
 const auto keyProfile=by5Env("BY5_KEY_PROFILE",0);by5::require(keyProfile<=1,"key profile must be 0 or 1");cfg.keyProfile=uint32_t(keyProfile);
 const uint64_t segStart=by5Env("BY5_SEGMENT_START",0);const bool prime=by5Env("BY5_PRIME",0)!=0;const bool fict=segStart>0||prime;const bool fmodel=by5Env("BY5_FICT_MODEL",0)!=0;
 by5::require(segStart<cfg.totalBatches(),"segment start beyond trace");
 uint64_t segBatches=by5Env("BY5_SEGMENT_BATCHES",0);if(!segBatches)segBatches=cfg.totalBatches()-segStart;
 by5::require(segBatches>0 && segStart+segBatches<=cfg.totalBatches(),"segment batches beyond trace");
 by5::require(by5::versionsPerSecond%cfg.batchesPerSecond()==0,"version step not integral");
 const int64_t step=int64_t(by5::versionsPerSecond/cfg.batchesPerSecond()),origin=int64_t(segStart)*step;
 by5::FictitiousHistoryConfig fcfg;fcfg.batches=int64_t(by5Env("BY5_PREHISTORY_BATCHES",57600));fcfg.versionStep=step;fcfg.minimumRealReadVersion=-int64_t(cfg.maxAge());fcfg.seed=by5Env("BY5_PREHISTORY_SEED",1);
 by5::FictitiousHistory fh(fcfg);const int64_t offset=fict?fh.realOffset():0;
 auto mapW=[&](Version v)->Version{if(!fict)return v;if(prime)return fh.writeVersion(v);by5::require(v>0,"real writes require positive versions");return offset+v;};
 auto mapR=[&](Version v)->Version{if(!fict)return v;if(prime)return fh.readVersion(v);by5::require(v>=fcfg.minimumRealReadVersion,"read precedes configured real history domain");return offset+v;};
 if(segStart)trace.seekg(std::streamoff(trace.tellg())+std::streamoff(segStart*cfg.batchSize*32));
 by5::require(bool(trace),"cannot seek to segment");
 bool validate=by5Env("BY5_VALIDATE",0)!=0;auto cpu=by5Env("BY5_CPU",6);by5::require(cpu<=65535,"CPU overflow");setAffinity(int(cpu));
 auto backend=selectedConflictHistory();by5::require(backend!=ConflictHistory::PagedSkipList,"unsupported backend");bool btree=backend==ConflictHistory::PagedBTree;by5::require(!btree,"btree-v1/v2 resolve with TempConflictBTree: run with --knob_resolver_use_paged_conflict_set=0");const bool capacity=true;
 if(btree){
  by5::require(paged_btree::PagedBTreeConflictSet::InlineKeyBytes==by5Env("BY5_INLINE_KEY_BYTES",64) && paged_btree::PagedBTreeConflictSet::Capacity==23,"inline key build mismatch or C != 23");
  by5::require(SERVER_KNOBS->PAGED_BTREE_READER_GROUP_SIZE==3 && SERVER_KNOBS->PAGED_RESOLVER_POOL_BACKING=="both","G3 and backing both required");
 }
 auto window=by5Env("BY5_WINDOW_US",capacity?0:5000000);by5::require(window==0,"btree-v1/v2 keep history by capacity: BY5_WINDOW_US must be 0");by5::require(window<=uint64_t(INT64_MAX),"window overflow");
 const char* logPath=std::getenv("BY5_BATCH_LOG");if(!logPath)logPath="by5resolvertest.csv";
 const size_t by5LogCap=size_t(by5Env("BY5_LOG_BUFFER_BYTES",268435456));static std::vector<char> by5LogBuf(by5LogCap),by5AgesBuf(by5LogCap);std::ofstream log,ages;log.rdbuf()->pubsetbuf(by5LogBuf.data(),std::streamsize(by5LogCap));ages.rdbuf()->pubsetbuf(by5AgesBuf.data(),std::streamsize(by5LogCap));log.open(logPath);ages.open(std::string(logPath)+".ages.csv");by5::require(bool(log)&&bool(ages),"cannot open logs");log<<std::setprecision(12);
 log<<"batch,measured,cv,window_floor,capacity_floor_before,capacity_floor_after,retained_us,accepted,conflicts,rejected_window,rejected_memory,target_conflicts,divergent,add_s,sort_s,read_s,intrabatch_s,combine_s,write_s,cleanup_s,total_s,pool_pages,free_pages,heap_pages,index_pages,nodes,leaves,reclaimed_pages,cumulative_peak_rss_kib,offered_hash,decisions_hash,raw_floor_after,model_trace_diff\n";
 ages<<"batch,measured,age_us,accepted,conflicts,rejected_window,rejected_memory,target_conflicts,divergent,read_width,write_width\n";
 std::ofstream blockLog;
 if(const char* bp=std::getenv("BY5_BLOCK_LOG");bp && *bp){by5::require(btree,"block log requires B+");blockLog.open(bp);by5::require(bool(blockLog),"cannot open block log");blockLog<<"batch,cv,floor,nodes,leaves,key_blocks,peak_key_blocks,free_blocks,reclaimed_key_units,node_bytes,max_record_bytes\n";}
 std::unique_ptr<ConflictSet> cs(newConflictSet());by5::History model;
 by5::require(std::strncmp(conflictSetIndexName(cs.get()),"btree-v",7)==0,"not a btree-v1/v2 binary");printf("by5tempbtree index=%s poolPages=%lld\n",conflictSetIndexName(cs.get()),(long long)SERVER_KNOBS->PAGED_RESOLVER_POOL_PAGES);
 printf("by5keyprofile profile=%u\n",cfg.keyProfile);
 if(btree)printf("by5blocks node_bytes=%zu max_record_bytes=%zu\n",cs->btreeHistory->layout().nodeBytes,cs->btreeHistory->maximumHeapRecordBytes());
 std::vector<By5Counts> totals(cfg.longAgesUs.size()+1);std::vector<double> times;times.reserve(cfg.measureSeconds*cfg.batchesPerSecond());
 double totalS=0,readS=0,writeS=0,cleanupS=0;bool everRejected=false;
 uint64_t offeredHash=14695981039346656037ULL,decisionsHash=14695981039346656037ULL;
 uint64_t hist[15]={};
 uint64_t modelTraceDiffEarly=0,modelTraceDiffLate=0;
 uint64_t fictBatch=0;std::ofstream fictReleases,fictLog;
 if(btree){
  by5::require(by5Env("BY5_FICT_LEAF",1)==1,"fictitious leaves are always enabled");
  by5::require(by5Env("BY5_FICT_RECLAIMABLE_PRIORITY",1)==1,"reclaimable priority is always enabled");
  by5::require(by5Env("BY5_FICT_STARTUP_ONLY",0)==0,"fictitious replenishment is always permanent");
  paged_btree::PagedBTreeConflictSet::FictitiousLeafConfig fc;
  fc.rootReserve=unsigned(by5Env("BY5_FICT_ROOT",fc.rootReserve));fc.splitReserve=unsigned(by5Env("BY5_FICT_SPLIT",fc.splitReserve));fc.choiceSeed=by5Env("BY5_FICT_SEED",fc.choiceSeed);
  fc.pressureMergedLimit=unsigned(by5Env("BY5_FICT_PRESSURE_LIMIT",fc.pressureMergedLimit));
  cs->btreeHistory->configureFictitiousLeaves(fc);
  printf("by5pressure mergedLimit=%u sourceLimit=%u\n",fc.pressureMergedLimit,paged_btree::PagedBTreeConflictSet::Capacity);
  printf("by5fictleaf rootReserve=%u splitReserve=%u choiceSeed=%llu\n",fc.rootReserve,fc.splitReserve,(unsigned long long)fc.choiceSeed);
  if(const char* rp=std::getenv("BY5_FICT_PRESSURE")){fictReleases.open(rp);fictReleases<<"batch,global,inspected,remove_left,remove_right,success\n";
   cs->btreeHistory->pressureObserver=[&](bool global,uint64_t inspected,unsigned removeLeft,unsigned removeRight,bool success){fictReleases<<fictBatch<<','<<int(global)<<','<<inspected<<','<<removeLeft<<','<<removeRight<<','<<int(success)<<'\n';};}
  if(const char* lp=std::getenv("BY5_FICT_LOG")){fictLog.open(lp);fictLog<<"batch,floor,created,released,fusion_consumed,retired,present,splits,pressure_events,local_attempts,global_attempts,local_fusions,global_fusions,nodes_made_available,fallbacks,inspected_sum,inspected_max,skipped_parent,skipped_limits,not_applied,age_candidates,merges,free_nodes,leaves,redist_left_free,redist_left_fict,redist_right_free,redist_right_fict,redist_incoming_moved,redist_cross_parent,redist_rejected_full,redist_rejected_slack,redist_no_neighbour,interval_insertions,pair_index_size,pair_updates,pair_flushes,pair_dirty_leaves,pair_dirty_max,pair_stale,pair_blocked,pair_empty";
   for(unsigned k=0;k<=paged_btree::PagedBTreeConflictSet::Capacity;++k)fictLog<<",real"<<k;for(unsigned k=0;k<=paged_btree::PagedBTreeConflictSet::Capacity;++k)fictLog<<",eff"<<k;fictLog<<"\n";}
 }
 printf("by5fictitious model=%d mode=%s segmentStart=%llu segmentBatches=%llu origin=%lld offset=%lld prehistoryBatches=%lld step=%lld minimumRealReadVersion=%lld\n",int(fmodel),!fict?"off":prime?"primed":"cold",(unsigned long long)segStart,(unsigned long long)segBatches,(long long)origin,(long long)offset,(long long)fcfg.batches,(long long)step,(long long)fcfg.minimumRealReadVersion);
 if(prime){
  by5::require(btree,"prime requires the B+ backend");
  const char* prePath=std::getenv("BY5_PREHISTORY_TRACE");by5::require(prePath && *prePath,"BY5_PREHISTORY_TRACE required");
  std::ifstream pre(prePath,std::ios::binary);auto pcfg=by5::readHeader(pre);
  by5::require(pcfg.rate==cfg.rate && pcfg.batchSize==cfg.batchSize && pcfg.shortAgeUs==cfg.shortAgeUs && pcfg.seed==cfg.seed && pcfg.conflictPpm==cfg.conflictPpm && pcfg.longPpm==cfg.longPpm && pcfg.domain==cfg.domain && pcfg.span==cfg.span && pcfg.prefixBytes==cfg.prefixBytes && pcfg.longAgesUs==cfg.longAgesUs,"prehistory workload configuration differs");
  by5::require(uint64_t(fcfg.batches)<=pcfg.totalBatches(),"prehistory trace too short");
  const char* plogPath=std::getenv("BY5_PREHISTORY_LOG");std::ofstream plog(plogPath?plogPath:"prehistory.csv");by5::require(bool(plog),"cannot open prehistory log");
  plog<<"batch,cv,floor,writes,nodes,leaves,free_pages,heap_pages,index_pages,reclaimed_pages,splits,internal_splits,merges,reclaim_calls,reclaim_age_candidates,expired_intervals,overwritten_intervals,nodes_taken\n";
  struct View{CommitTransactionRef* p;size_t n;CommitTransactionRef* data(){return p;}size_t size()const{return n;}};
  struct Primer{ConflictSet* cs;
   Version validationMinRV()const{return getConflictSetStats(cs).validationMinRV;}
   int64_t nodeCount()const{return getConflictSetStats(cs).btreeNodes;}
   void addWrites(int64_t cv,CommitTransactionRef* p,size_t n){ConflictBatch batch(cs);for(size_t i=0;i<n;++i)batch.addTransaction(p[i],0);std::vector<int> acc,old;batch.detectConflicts(cv,0,acc,&old);by5::require(acc.size()==n && old.empty(),"prehistory write rejected");}
  } primer{cs.get()};
  Arena parena;std::vector<CommitTransactionRef> ptrs;ptrs.reserve(cfg.batchSize);std::vector<std::array<std::string,2>> pkeys(cfg.batchSize);
  uint64_t syntheticWrites=0;double setupStart=timer();
  fh.preload(primer,[&](int64_t pb,uint64_t){
    Version pnow=by5::commitVersion(pcfg,uint64_t(pb));by5::require(pnow==(pb+1)*step,"prehistory version mapping");
    parena=Arena();ptrs.clear();
    auto ref=[](const std::string&x){return StringRef(reinterpret_cast<const uint8_t*>(x.data()),int(x.size()));};
    for(size_t i=0;i<pcfg.batchSize;++i){auto t=by5::readTransaction(pre,pcfg,pnow);if(t.expectedConflict)continue;
     auto&k=pkeys[ptrs.size()];auto keyCfg=pcfg;keyCfg.keyProfile=cfg.keyProfile;k[0]=by5::key(keyCfg,t.write.begin);k[1]=by5::key(keyCfg,t.write.end);
     CommitTransactionRef tr;tr.read_snapshot=t.readVersion;tr.write_conflict_ranges.push_back(parena,KeyRangeRef(ref(k[0]),ref(k[1])));ptrs.push_back(tr);}
    syntheticWrites+=ptrs.size();return View{ptrs.data(),ptrs.size()};
   },[&](int64_t pb,int64_t cv,const Primer&){
    if(pb%32!=31 && pb+1!=fcfg.batches)return;
    auto st=getConflictSetStats(cs.get());const auto&ts=cs->btreeHistory->stats();
    plog<<pb<<','<<cv<<','<<st.validationMinRV<<','<<syntheticWrites<<','<<st.btreeNodes<<','<<st.btreeLeaves<<','<<st.poolFreePages<<','<<st.poolHeapPages<<','<<st.poolIndexPages<<','<<st.reclaimedHistoryPages<<','<<ts.splits<<','<<ts.internalSplits<<','<<ts.merges<<','<<ts.reclaimCalls<<','<<ts.reclaimAgeCandidates<<','<<ts.expiredIntervals<<','<<ts.overwrittenIntervals<<','<<ts.nodesTaken<<'\n'<<std::flush;
   });
  by5::require(fh.ready() && fh.realOffset()==offset,"prehistory incomplete");
  auto st=getConflictSetStats(cs.get());
  printf("by5prehistory batches=%lld syntheticWrites=%llu setupS=%.3f syntheticEnd=%lld rawFloor=%lld translatedFloor=%lld nodes=%lld leaves=%lld freePages=%lld\n",(long long)fh.completedBatches(),(unsigned long long)syntheticWrites,timer()-setupStart,(long long)fh.syntheticEnd(),(long long)st.validationMinRV,(long long)(st.validationMinRV-offset),(long long)st.btreeNodes,(long long)st.btreeLeaves,(long long)st.poolFreePages);
 }
 printf("by5resolvertest format=2 backend=%s rate=%llu batch=%llu shortAgeUs=%llu longPpm=%llu conflictPpm=%llu prefix=%u domain=%u warmupSeconds=%llu measureSeconds=%llu windowUs=%llu cpu=%llu validate=%d\n",conflictSetIndexName(cs.get()),(unsigned long long)cfg.rate,(unsigned long long)cfg.batchSize,(unsigned long long)cfg.shortAgeUs,(unsigned long long)cfg.longPpm,(unsigned long long)cfg.conflictPpm,cfg.prefixBytes,cfg.domain,(unsigned long long)cfg.warmupSeconds,(unsigned long long)cfg.measureSeconds,(unsigned long long)window,(unsigned long long)cpu,int(validate));
#ifdef __linux__
 printf("by5resolvertest running=%d\n",sched_getcpu());
#endif
 if(cfg.pattern)printf("by5monotonic pattern=%u prefixes=%u pointWrites=1 pointReadsWidth1=1\n",cfg.pattern,cfg.prefixes);
 printf("by5resolvertest memory_scope=pool_cap_for_btree,no_byte_cap_for_fdb timing_valid=%d\n",int(!validate));
 for(uint64_t b=0;b<(fict?segBatches:cfg.totalBatches());++b){
  bool measured=fict?b+cfg.measureSeconds*cfg.batchesPerSecond()>=segBatches:b>=cfg.warmupBatches();Version fileNow=by5::commitVersion(cfg,b+segStart),realNow=fileNow-origin;Version now=mapW(realNow),floor=window?std::max<Version>(0,now-Version(window)):0;
  std::vector<Version> rvs;rvs.reserve(cfg.batchSize);uint64_t batchDiff=0;
  fictBatch=b;
  std::vector<by5::Transaction> generated;generated.reserve(cfg.batchSize);
  Arena arena;std::vector<CommitTransactionRef> transactions;transactions.reserve(cfg.batchSize);
  struct Keys{std::string read,endRead,write,endWrite;};std::vector<Keys> keys(cfg.batchSize);
  uint64_t batchHash=14695981039346656037ULL;
  for(size_t i=0;i<cfg.batchSize;++i){
   generated.push_back(by5::readTransaction(trace,cfg,fileNow));const auto&t=generated.back();auto&k=keys[i];rvs.push_back(fict?mapR(t.readVersion-origin):t.readVersion);
   if(by5::hasRead(t.read)){k.read=by5::key(cfg,t.read.begin);k.endRead=cfg.pattern && t.read.end==t.read.begin+1?k.read+std::string(1,'\0'):by5::key(cfg,t.read.end);}
   k.write=by5::key(cfg,t.write.begin);k.endWrite=cfg.pattern?k.write+std::string(1,'\0'):by5::key(cfg,t.write.end);
   auto ref=[](const std::string&s){return StringRef(reinterpret_cast<const uint8_t*>(s.data()),int(s.size()));};
   CommitTransactionRef tr;tr.read_snapshot=rvs.back();
   if(by5::hasRead(t.read))tr.read_conflict_ranges.push_back(arena,KeyRangeRef(ref(k.read),ref(k.endRead)));tr.write_conflict_ranges.push_back(arena,KeyRangeRef(ref(k.write),ref(k.endWrite)));transactions.push_back(tr);
   for(const auto*s:{&k.read,&k.endRead,&k.write,&k.endWrite}){by5::hashWord(batchHash,s->size());for(unsigned char ch:*s)by5::hashWord(batchHash,ch);if(measured && s->size()>=16)++hist[(s->size()-16)/8];}
   by5::hashWord(batchHash,uint64_t(t.readVersion));by5::hashWord(batchHash,t.ageClass);by5::hashWord(batchHash,t.expectedConflict);
  }
  auto before=getConflictSetStats(cs.get());double sortBefore=g_sort.getValue();
  std::vector<int> accepted,tooOld;double start=timer(),add;
  {ConflictBatch batch(cs.get());for(auto&tr:transactions)batch.addTransaction(tr,floor);add=timer()-start;batch.detectConflicts(now,floor,accepted,&tooOld);}
  double elapsed=timer()-start;auto after=getConflictSetStats(cs.get());
  if(blockLog.is_open() && b%32==31){const auto& tr=*cs->btreeHistory;blockLog<<b<<','<<now<<','<<tr.validationMinRV()<<','<<tr.nodeCount()<<','<<tr.leafCount()<<','<<tr.keyBlockCount()<<','<<tr.peakKeyBlockCount()<<','<<tr.freeNodeCount()<<','<<tr.reclaimedHistoryPages()<<','<<tr.layout().nodeBytes<<','<<tr.maximumHeapRecordBytes()<<'\n';by5::require(bool(blockLog),"block log write failed");}
  if(fictLog.is_open() && b%32==31){const auto& tr=*cs->btreeHistory;const auto& fs=tr.stats();std::array<uint64_t,paged_btree::PagedBTreeConflictSet::Capacity+1> rh,eh;uint64_t present=0;tr.leafOccupancy(rh,eh,present);
   fictLog<<b<<','<<after.validationMinRV<<','<<fs.fictitiousCreated<<','<<fs.fictitiousReleased<<','<<fs.fictitiousFusionConsumed<<','<<fs.fictitiousRetired<<','<<present<<','<<fs.fictitiousSplits<<','<<fs.pressureEvents<<','<<fs.pressureLocalAttempts<<','<<fs.pressureGlobalAttempts<<','<<fs.pressureLocalFusions<<','<<fs.pressureGlobalFusions<<','<<fs.pressureNodesMadeAvailable<<','<<fs.pressureFallbacks<<','<<fs.pressureCandidatesInspected<<','<<fs.pressureCandidatesMax<<','<<fs.pressureSkippedNotSameParent<<','<<fs.pressureSkippedLimits<<','<<fs.pressureFusionNotApplied<<','<<fs.reclaimAgeCandidates<<','<<fs.merges<<','<<tr.freeNodeCount()<<','<<tr.leafCount()<<','<<fs.redistLeftFree<<','<<fs.redistLeftFictitious<<','<<fs.redistRightFree<<','<<fs.redistRightFictitious<<','<<fs.redistIncomingMoved<<','<<fs.redistCrossParent<<','<<fs.redistRejectedFull<<','<<fs.redistRejectedSlack<<','<<fs.redistNoNeighbour<<','<<fs.intervalInsertions<<','<<tr.pairIndexSize()<<','<<fs.pairIndexUpdates<<','<<fs.pairIndexFlushes<<','<<fs.pairIndexDirtyLeaves<<','<<fs.pairIndexDirtyMax<<','<<fs.pairIndexStale<<','<<fs.pairIndexBlocked<<','<<fs.pairIndexEmpty;
   for(auto v:rh)fictLog<<','<<v;for(auto v:eh)fictLog<<','<<v;fictLog<<'\n'<<std::flush;}
  std::vector<uint8_t> status(cfg.batchSize,1);
  for(int i:accepted){ASSERT(i>=0 && uint64_t(i)<cfg.batchSize && status[i]==1);status[i]=0;}
  for(int i:tooOld){ASSERT(i>=0 && uint64_t(i)<cfg.batchSize && status[i]==1);status[i]=generated[i].readVersion<floor?2:3;}
  if(!tooOld.empty())everRejected=true;
  std::vector<By5Counts> counts(totals.size());By5Counts all;
  for(size_t i=0;i<generated.size();++i){const auto&t=generated[i];auto&c=counts[t.ageClass];
   bool wr=rvs[i]<floor,mr=!wr && capacity && rvs[i]<before.validationMinRV;
   ASSERT((status[i]==2)==wr);ASSERT((status[i]==3)==mr);
   if(fict && fmodel){bool mc=by5::readConflicts(model,t.read,rvs[i]);batchDiff+=mc!=t.expectedConflict;if(status[i]<2)ASSERT((status[i]==1)==mc);}
   else if(fict){if(status[i]<2 && !everRejected && uint64_t(realNow)>cfg.maxAge())ASSERT((status[i]==1)==t.expectedConflict);}
   else if(status[i]<2){if(!everRejected)ASSERT((status[i]==1)==t.expectedConflict);if(validate)ASSERT((status[i]==1)==by5::readConflicts(model,t.read,t.readVersion));}
   if((validate||(fict && fmodel)) && status[i]==0)model.write(t.write,now);
   for(auto*p:{&c,&all}){p->accepted+=status[i]==0;p->conflicts+=status[i]==1;p->window+=status[i]==2;p->memory+=status[i]==3;p->target+=t.expectedConflict;p->divergent+=status[i]<2 && (status[i]==1)!=t.expectedConflict;p->readWidth+=t.read.end-t.read.begin;p->writeWidth+=t.write.end-t.write.begin;by5::hashWord(p->hash,status[i]);}
  }
  uint64_t rss=0;
#ifdef __linux__
  struct rusage usage;if(getrusage(RUSAGE_SELF,&usage)==0)rss=uint64_t(usage.ru_maxrss);
#endif
  double read=after.checkReadSeconds-before.checkReadSeconds,write=after.mergeSeconds-before.mergeSeconds,cleanup=after.cleanupSeconds-before.cleanupSeconds;
  if(fict){if(uint64_t(realNow)<=cfg.maxAge())modelTraceDiffEarly+=batchDiff;else modelTraceDiffLate+=batchDiff;}
  log<<b<<','<<measured<<','<<(fict?realNow:now)<<','<<floor<<','<<(fict?before.validationMinRV-offset:before.validationMinRV)<<','<<(fict?after.validationMinRV-offset:after.validationMinRV)<<','<<(fict?realNow-std::max<Version>(0,after.validationMinRV-offset):capacity?now-after.validationMinRV:now-floor)<<','<<all.accepted<<','<<all.conflicts<<','<<all.window<<','<<all.memory<<','<<all.target<<','<<all.divergent<<','<<add<<','<<g_sort.getValue()-sortBefore<<','<<read<<','<<after.checkIntraBatchSeconds-before.checkIntraBatchSeconds<<','<<after.combineSeconds-before.combineSeconds<<','<<write<<','<<cleanup<<','<<elapsed<<','<<after.poolLimitPages<<','<<after.poolFreePages<<','<<after.poolHeapPages<<','<<after.poolIndexPages<<','<<after.btreeNodes<<','<<after.btreeLeaves<<','<<after.reclaimedHistoryPages<<','<<rss<<','<<batchHash<<','<<all.hash<<','<<after.validationMinRV<<','<<batchDiff<<'\n';
  for(size_t cls=0;cls<counts.size();++cls){auto&c=counts[cls];ages<<b<<','<<measured<<','<<(cls?cfg.longAgesUs[cls-1]:cfg.shortAgeUs)<<','<<c.accepted<<','<<c.conflicts<<','<<c.window<<','<<c.memory<<','<<c.target<<','<<c.divergent<<','<<c.readWidth<<','<<c.writeWidth<<'\n';
   if(measured){auto&t=totals[cls];t.accepted+=c.accepted;t.conflicts+=c.conflicts;t.window+=c.window;t.memory+=c.memory;t.target+=c.target;t.divergent+=c.divergent;}
  }
  by5::require(bool(log)&&bool(ages),"log write failed");
  if(measured){totalS+=elapsed;readS+=read;writeS+=write;cleanupS+=cleanup;times.push_back(elapsed);by5::hashWord(offeredHash,batchHash);by5::hashWord(decisionsHash,all.hash);}
 }
 if(!fict || segStart+segBatches==cfg.totalBatches())by5::require(trace.peek()==EOF,"extra trace data");
 if(btree)printf("by5blocksresult blocks=%llu peak=%llu reclaimed=%llu\n",(unsigned long long)cs->btreeHistory->keyBlockCount(),(unsigned long long)cs->btreeHistory->peakKeyBlockCount(),(unsigned long long)cs->btreeHistory->reclaimedHistoryPages());
 printf("by5fictitiousresult modelTraceDiffEarly=%llu modelTraceDiffLate=%llu\n",(unsigned long long)modelTraceDiffEarly,(unsigned long long)modelTraceDiffLate);std::sort(times.begin(),times.end());
 auto percentile=[&](double p){return times[size_t(p*(times.size()-1))]*1000;};uint64_t accepted=0;for(const auto&t:totals)accepted+=t.accepted;
 printf("by5result offered=%llu accepted=%llu offeredHash=%016llx decisionsHash=%016llx totalS=%.9f readS=%.9f writeS=%.9f cleanupS=%.9f p50ms=%.6f p95ms=%.6f p99ms=%.6f usPerOffered=%.6f usPerAccepted=%.6f\n",(unsigned long long)(cfg.measureSeconds*cfg.rate),(unsigned long long)accepted,(unsigned long long)offeredHash,(unsigned long long)decisionsHash,totalS,readS,writeS,cleanupS,percentile(.5),percentile(.95),percentile(.99),totalS*1e6/(cfg.measureSeconds*cfg.rate),accepted?totalS*1e6/accepted:0);
 for(size_t i=0;i<totals.size();++i){const auto&t=totals[i];printf("by5age ageUs=%llu accepted=%llu conflicts=%llu rejectedWindow=%llu rejectedMemory=%llu target=%llu divergent=%llu\n",(unsigned long long)(i?cfg.longAgesUs[i-1]:cfg.shortAgeUs),(unsigned long long)t.accepted,(unsigned long long)t.conflicts,(unsigned long long)t.window,(unsigned long long)t.memory,(unsigned long long)t.target,(unsigned long long)t.divergent);}
 {const long long lb=(long long)log.tellp(),ab=(long long)ages.tellp();printf("by5logbuffer batches_bytes=%lld ages_bytes=%lld capacity=%zu\n",lb,ab,by5LogCap);by5::require(lb>=0&&ab>=0&&size_t(lb)<by5LogCap&&size_t(ab)<by5LogCap,"log buffer too small");}
 printf("by5lengths");if(cfg.keyProfile){for(unsigned length:{16u,24u,32u,48u,64u,100u,128u})printf(" %u=%llu",length,(unsigned long long)hist[(length-16)/8]);}else for(unsigned i=0;i<7;++i)printf(" %u=%llu",16+8*i,(unsigned long long)hist[i]);printf("\n");
}
