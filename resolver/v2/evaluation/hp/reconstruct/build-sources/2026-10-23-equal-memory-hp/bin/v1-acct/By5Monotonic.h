#pragma once
#include "By5Workload.h"
#include <unordered_set>
namespace by5 {
inline Config monotonicConfig(uint32_t prefixes,uint64_t seconds,uint64_t measure,uint64_t seed){
 Config c;c.pattern=1;c.prefixes=prefixes;c.longPpm=0;c.conflictPpm=0;c.seed=seed;c.domain=0xFFFFFFF0u;c.span=10;c.prefixBytes=12;
 require(measure>0 && seconds>measure,"invalid monotonic durations");
 c.warmupSeconds=seconds-measure;c.measureSeconds=measure;c.validate();return c;
}
inline uint32_t prefixOf(const Config& c,uint32_t rank){return uint32_t(uint64_t(rank)/c.prefixCapacity());}
inline uint64_t prefixLimit(const Config& c,uint32_t p){
 uint64_t end=std::min<uint64_t>(uint64_t(c.prefixBase(p))+c.prefixCapacity(),c.domain);return end-c.prefixBase(p);
}
class MonotonicGenerator {
 Config cfg;Random rng;History oracle;uint64_t position=0;
 std::vector<uint64_t> next;std::unordered_set<uint32_t> rejected;
public:
 static constexpr unsigned pointTries=64;
 uint64_t emptyReads=0,clampedReads=0,pointReads=0,rangeReads=0,pointRetries=0,conflicts=0;
 std::vector<uint64_t> assignedPerPrefix,acceptedPerPrefix,readsPerPrefix;
 explicit MonotonicGenerator(Config c):cfg(c),rng{c.seed},next(c.prefixes,0),assignedPerPrefix(c.prefixes,0),acceptedPerPrefix(c.prefixes,0),readsPerPrefix(c.prefixes,0){
  cfg.validate();require(cfg.pattern==1,"monotonic pattern required");
 }
 const Config& config()const{return cfg;}
 size_t historySegments()const{return oracle.size();}
 const std::vector<uint64_t>& inserted()const{return next;}
 std::vector<Transaction> batch(uint64_t b){
  require(position==b*cfg.batchSize,"nonconsecutive generation");
  const Version now=commitVersion(cfg,b),rv=std::max<Version>(0,now-Version(cfg.shortAgeUs));
  const std::vector<uint64_t> available=next;
  std::vector<uint32_t> readable;
  for(uint32_t p=0;p<cfg.prefixes;++p)if(available[p])readable.push_back(p);
  std::vector<Transaction> out;out.reserve(cfg.batchSize);
  for(uint64_t i=0;i<cfg.batchSize;++i){
   const uint32_t wp=cfg.prefixes==1?0:uint32_t(rng.below(cfg.prefixes));
   require(next[wp]<prefixLimit(cfg,wp),"prefix counter overflow");
   const uint32_t wr=cfg.prefixBase(wp)+uint32_t(next[wp]++);++assignedPerPrefix[wp];
   Range read{0,0};
   if(!readable.empty()){
    const uint32_t rp=readable.size()==1?readable[0]:readable[rng.below(readable.size())];
    const uint64_t n=available[rp];uint64_t w=1+rng.below(cfg.span);
    if(w>n){w=n;++clampedReads;}
    const uint32_t base=cfg.prefixBase(rp);
    if(w==1){
     bool found=false;
     for(unsigned tries=0;tries<pointTries;++tries){
      const uint32_t r=base+uint32_t(rng.below(n));
      if(!rejected.count(r)){read={r,r+1};found=true;break;}
      ++pointRetries;
     }
     require(found,"no inserted key found for a point read");++pointReads;++readsPerPrefix[rp];
    }else{
     const uint32_t start=base+uint32_t(rng.below(n-w+1));
     read={start,start+uint32_t(w)};++rangeReads;++readsPerPrefix[rp];
    }
   }
   if(!hasRead(read))++emptyReads;
   const bool conflict=readConflicts(oracle,read,rv);
   if(conflict){rejected.insert(wr);++conflicts;}else{oracle.write({wr,wr+1},now);++acceptedPerPrefix[wp];}
   out.push_back({read,{wr,wr+1},rv,0,conflict});
   ++position;
  }
  return out;
 }
};
}
