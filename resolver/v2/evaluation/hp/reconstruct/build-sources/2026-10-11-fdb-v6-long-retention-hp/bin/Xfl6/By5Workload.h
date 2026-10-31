#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>
namespace by5 {
using Version=int64_t;
constexpr uint64_t versionsPerSecond=1000000;
inline void require(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
struct Config {
 uint64_t rate=65536,batchSize=2048,shortAgeUs=1562500,warmupSeconds=60,measureSeconds=120,seed=1,conflictPpm=7300,longPpm=1000;
 uint32_t domain=20000000,span=10,prefixBytes=12;
 std::vector<uint64_t> longAgesUs{10000000,20000000,30000000};
 uint64_t batchesPerSecond()const{return rate/batchSize;}
 uint64_t warmupBatches()const{return warmupSeconds*batchesPerSecond();}
 uint64_t totalBatches()const{return (warmupSeconds+measureSeconds)*batchesPerSecond();}
 uint64_t maxAge()const{return longPpm?longAgesUs.back():shortAgeUs;}
 void validate()const{
  require(batchSize>=2 && batchSize<=65536 && rate>=batchSize && rate<=1000000000 && rate%batchSize==0,"invalid rate/batch");
  require(warmupSeconds<=1000000 && measureSeconds>0 && measureSeconds<=1000000,"invalid durations");
  require(shortAgeUs>0 && shortAgeUs<=1000000000000ULL,"invalid short age");
  require(conflictPpm<=1000000 && longPpm<=1000000 && prefixBytes<=12 && span>0,"invalid rates/layout");
  require(!longPpm || !longAgesUs.empty(),"long ages required");
  uint64_t previous=shortAgeUs;
  for(auto a:longAgesUs){require(a>previous && a<=1000000000000ULL,"long ages must increase above short age");previous=a;}
  require(warmupSeconds*versionsPerSecond>=maxAge(),"warmup must cover largest age");
  require(uint64_t(span)<=(UINT64_MAX-shortAgeUs+1)/maxAge(),"span overflow");
  uint64_t largest=(uint64_t(span)*maxAge()+shortAgeUs-1)/shortAgeUs;
  require(domain>largest && uint64_t(domain)+largest<=UINT32_MAX,"domain too small / endpoint overflow");
 }
};
struct Random {
 uint64_t state;
 uint64_t next(){uint64_t z=(state+=0x9e3779b97f4a7c15ULL);z=(z^(z>>30))*0xbf58476d1ce4e5b9ULL;z=(z^(z>>27))*0x94d049bb133111ebULL;return z^(z>>31);}
 uint64_t below(uint64_t n){require(n>0,"empty domain");uint64_t x,threshold=(-n)%n;do{x=next();}while(x<threshold);return x%n;}
};
inline std::string key(const Config& c,uint32_t rank){
 std::string out(16+8*(rank%7),'a');
 for(unsigned j=0;j<4;++j)out[c.prefixBytes+j]=char(rank>>(24-8*j));
 return out;
}
inline Version commitVersion(const Config& c,uint64_t b){return Version((b+1)*versionsPerSecond/c.batchesPerSecond());}
struct Range{uint32_t begin,end;};
struct Transaction{Range read,write;Version readVersion;uint32_t ageClass;bool expectedConflict;};
class History {
 std::map<uint32_t,Version> segments{{0,0}};
 std::map<uint32_t,Version>::iterator split(uint32_t at){
  auto lo=std::prev(segments.upper_bound(at));
  return lo->first==at?lo:segments.emplace_hint(std::next(lo),at,lo->second);
 }
public:
 bool conflicts(Range r,Version rv)const{
  auto it=std::prev(segments.upper_bound(r.begin));
  do{if(it->second>rv)return true;++it;}while(it!=segments.end() && it->first<r.end);
  return false;
 }
 void write(Range r,Version now){
  auto end=split(r.end),begin=split(r.begin);
  segments.erase(std::next(begin),end);begin->second=now;
  if(begin!=segments.begin() && std::prev(begin)->second==now)segments.erase(begin);
  if(end!=segments.begin() && std::prev(end)->second==end->second)segments.erase(end);
 }
 size_t size()const{return segments.size();}
};
inline uint32_t width(const Config& c,Random& rng,uint64_t age){
 uint64_t n=(1+rng.below(c.span))*age,result=n/c.shortAgeUs;
 if(rng.below(c.shortAgeUs)<n%c.shortAgeUs)++result;
 require(result>0 && result<=UINT32_MAX,"width overflow");return uint32_t(result);
}
class Generator {
 Config cfg;Random rng;History oracle;uint64_t longOrdinal=0,position=0;
 std::vector<uint64_t> classCount;
public:
 uint64_t candidateTrials=0,bootstrapExceptions=0;
 explicit Generator(Config c):cfg(c),rng{c.seed},classCount(c.longAgesUs.size()+1,0){cfg.validate();}
 size_t historySegments()const{return oracle.size();}
 std::vector<Transaction> batch(uint64_t b){
  require(position==b*cfg.batchSize,"nonconsecutive generation");
  uint64_t nlong=((position+cfg.batchSize)*cfg.longPpm)/1000000-(position*cfg.longPpm)/1000000;
  std::vector<uint32_t> classes(cfg.batchSize,0);
  for(uint64_t i=0;i<nlong;++i)classes[i]=1+(longOrdinal++%cfg.longAgesUs.size());
  for(size_t i=classes.size();i>1;--i)std::swap(classes[i-1],classes[rng.below(i)]);
  std::vector<Transaction> out;out.reserve(cfg.batchSize);Version now=commitVersion(cfg,b);
  for(auto cls:classes){
   uint64_t age=cls?cfg.longAgesUs[cls-1]:cfg.shortAgeUs;
   Version rv=std::max<Version>(0,now-Version(age));uint64_t count=classCount[cls]++;
   bool target=((count+1)*cfg.conflictPpm)/1000000!=(count*cfg.conflictPpm)/1000000;
   if(position==0 && target){target=false;++bootstrapExceptions;}
   uint32_t rw=width(cfg,rng,age),ww=width(cfg,rng,age);Range read{};bool found=false;
   for(unsigned tries=0;tries<1000000;++tries){
    uint32_t start=uint32_t(rng.below(cfg.domain));read={start,start+rw};++candidateTrials;
    if(oracle.conflicts(read,rv)==target){found=true;break;}
   }
   require(found,"target cannot be sampled at fixed width: enlarge domain; no biased fallback");
   uint32_t start=uint32_t(rng.below(cfg.domain));Range write{start,start+ww};
   out.push_back({read,write,rv,cls,target});if(!target)oracle.write(write,now);++position;
  }
  return out;
 }
};
inline void hashWord(uint64_t& h,uint64_t v){for(unsigned i=0;i<8;++i){h^=uint8_t(v>>(8*i));h*=1099511628211ULL;}}
inline void put(std::ostream& s,uint64_t n,unsigned bytes=8){for(unsigned i=0;i<bytes;++i)s.put(char(n>>(8*i)));require(bool(s),"trace write failed");}
inline uint64_t get(std::istream& s,unsigned bytes=8){uint64_t n=0;for(unsigned i=0;i<bytes;++i){int c=s.get();require(c!=EOF,"truncated trace");n|=uint64_t(uint8_t(c))<<(8*i);}return n;}
inline void writeHeader(std::ostream& s,const Config& c){
 s.write("BY5RNG02",8);
 for(auto v:{c.rate,c.batchSize,c.shortAgeUs,c.warmupSeconds,c.measureSeconds,c.seed,c.conflictPpm,c.longPpm,uint64_t(c.domain),uint64_t(c.span),uint64_t(c.prefixBytes),uint64_t(c.longAgesUs.size())})put(s,v);
 for(auto a:c.longAgesUs)put(s,a);
}
inline Config readHeader(std::istream& s){
 char magic[8]{};s.read(magic,8);require(std::string(magic,8)=="BY5RNG02","wrong trace format");Config c;
 c.rate=get(s);c.batchSize=get(s);c.shortAgeUs=get(s);c.warmupSeconds=get(s);c.measureSeconds=get(s);c.seed=get(s);c.conflictPpm=get(s);c.longPpm=get(s);
 auto d=get(s),w=get(s),p=get(s),n=get(s);require(d<=UINT32_MAX && w<=UINT32_MAX && p<=12 && n<=128,"invalid header");
 c.domain=uint32_t(d);c.span=uint32_t(w);c.prefixBytes=uint32_t(p);c.longAgesUs.clear();for(uint64_t i=0;i<n;++i)c.longAgesUs.push_back(get(s));c.validate();return c;
}
inline void writeTransaction(std::ostream& s,const Transaction& t){
 put(s,t.read.begin,4);put(s,t.read.end,4);put(s,t.write.begin,4);put(s,t.write.end,4);put(s,uint64_t(t.readVersion));put(s,t.ageClass,4);put(s,t.expectedConflict,4);
}
inline Transaction readTransaction(std::istream& s,const Config& c,Version now){
 Transaction t;t.read={uint32_t(get(s,4)),uint32_t(get(s,4))};t.write={uint32_t(get(s,4)),uint32_t(get(s,4))};t.readVersion=Version(get(s));t.ageClass=uint32_t(get(s,4));auto target=get(s,4);require(target<=1,"invalid target");t.expectedConflict=target;
 require(t.ageClass<=c.longAgesUs.size() && t.read.begin<c.domain && t.write.begin<c.domain && t.read.end>t.read.begin && t.write.end>t.write.begin,"invalid range/class");
 uint64_t age=t.ageClass?c.longAgesUs[t.ageClass-1]:c.shortAgeUs;
 require(t.readVersion==std::max<Version>(0,now-Version(age)),"invalid RV");return t;
}
}
