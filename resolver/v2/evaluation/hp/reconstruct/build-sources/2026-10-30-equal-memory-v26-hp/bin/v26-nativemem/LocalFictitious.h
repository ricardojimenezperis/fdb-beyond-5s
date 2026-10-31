#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <stdexcept>
namespace by5fictitious4 {
constexpr std::uint32_t none=std::numeric_limits<std::uint32_t>::max();
struct Links {
    std::uint8_t fictitious=0;
    std::uint32_t previous=none,next=none;
};
template<unsigned C=23> class Buckets {
    static_assert(C>0 && C<=31);
    std::array<std::uint32_t,C+1> heads;
    std::uint32_t mask=0;
public:
    Buckets() { heads.fill(none); }
    unsigned highest() const {
        for(unsigned n=C;n;--n) if(mask&(std::uint32_t(1)<<n)) return n;
        return 0;
    }
    std::uint32_t head(unsigned n) const {
        if(n>C) throw std::invalid_argument("bucket");
        return heads[n];
    }
    template<class Lookup,class TryCandidate>
    bool visitDescending(Lookup&& get,TryCandidate&& attempt,std::uint64_t& inspected) {
        for(unsigned n=highest();n;--n) {
            for(auto id=heads[n];id!=none;) {
                auto next=get(id).next;
                ++inspected;
                if(attempt(id)) return true;
                id=next;
            }
        }
        return false;
    }
    template<class Lookup> void set(std::uint32_t id,unsigned count,Lookup&& get) {
        if(id==none || count>C) throw std::invalid_argument("membership");
        auto& x=get(id);
        unsigned old=x.fictitious;
        if(old>C) throw std::logic_error("corrupt counter");
        if(old) {
            if(x.previous!=none) get(x.previous).next=x.next;
            else {
                if(heads[old]!=id) throw std::logic_error("unregistered leaf");
                heads[old]=x.next;
            }
            if(x.next!=none) get(x.next).previous=x.previous;
            if(heads[old]==none) mask&=~(std::uint32_t(1)<<old);
        } else if(x.previous!=none || x.next!=none) throw std::logic_error("zero-count membership");
        x.fictitious=static_cast<std::uint8_t>(count);
        x.previous=x.next=none;
        if(count) {
            x.next=heads[count];
            if(x.next!=none) get(x.next).previous=id;
            heads[count]=id;
            mask|=std::uint32_t(1)<<count;
        }
    }
};
struct FusionPlan { bool possible=false; unsigned removeLeft=0,removeRight=0; };
template<unsigned C=23> struct Policy {
    struct Split { unsigned leftReal,rightReal,leftFictitious,rightFictitious; bool insertRight; };
    static unsigned occupancy(unsigned real,unsigned fictitious) {
        if(real>C || fictitious>C-real) throw std::invalid_argument("occupancy");
        return real+fictitious;
    }
    static bool needsSingleRelease(unsigned real,unsigned fictitious) {
        return occupancy(real,fictitious)==C && fictitious>0;
    }
    static FusionPlan planFusion(unsigned lr,unsigned lf,unsigned rr,unsigned rf,bool sameParent,
                                 unsigned mergedLimit=17,unsigned sourceLimit=6) {
        occupancy(lr,lf);occupancy(rr,rf);
        if(!mergedLimit || mergedLimit>C || sourceLimit>C) throw std::invalid_argument("fusion limits");
        if(!sameParent || !lr || !rr || lr+rr>mergedLimit || std::min(lr,rr)>sourceLimit) return {};
        unsigned sum=lr+lf+rr+rf;
        unsigned needed=sum>mergedLimit?sum-mergedLimit:0;
        FusionPlan best;
        unsigned bestRemoved=2*C+1;
        for(unsigned side=0;side<2;++side) {
            if((side?rr:lr)>sourceLimit) continue;
            unsigned a=0,b=0;
            if(side==0) a=lr+lf>sourceLimit?lr+lf-sourceLimit:0;
            else b=rr+rf>sourceLimit?rr+rf-sourceLimit:0;
            unsigned remaining=needed>a+b?needed-a-b:0;
            unsigned da=std::min(remaining,lf-a);a+=da;remaining-=da;
            unsigned db=std::min(remaining,rf-b);b+=db;remaining-=db;
            if(remaining) continue;
            if(a+b<bestRemoved) { best={true,a,b};bestRemoved=a+b; }
        }
        return best;
    }
    static Split split(unsigned real,unsigned fictitious,unsigned rank,unsigned addedReserve,std::uint64_t choice) {
        occupancy(real,fictitious);
        if(real!=C || fictitious || rank>real || addedReserve>=C) throw std::invalid_argument("release before splitting");
        unsigned peakBest=2*C+1,gapBest=2*C+1,count=0;
        for(unsigned pass=0;pass<2;++pass) {
            unsigned selected=pass?static_cast<unsigned>(choice%count):0;
            for(unsigned left=1;left<real;++left) for(unsigned lf=0;lf<=addedReserve;++lf) {
                bool right=rank>=left;
                unsigned rf=addedReserve-lf,a=left+lf+!right,b=real-left+rf+right;
                if(a>=C || b>=C) continue;
                unsigned peak=std::max(a,b),gap=std::max(a,b)-std::min(a,b);
                if(!pass) {
                    if(peak<peakBest || (peak==peakBest && gap<gapBest)) { peakBest=peak;gapBest=gap;count=0; }
                    if(peak==peakBest && gap==gapBest) ++count;
                } else if(peak==peakBest && gap==gapBest && !selected--) return {left,real-left,lf,rf,right};
            }
            if(!count) throw std::runtime_error("no split");
        }
        throw std::logic_error("split selection");
    }
};
}
