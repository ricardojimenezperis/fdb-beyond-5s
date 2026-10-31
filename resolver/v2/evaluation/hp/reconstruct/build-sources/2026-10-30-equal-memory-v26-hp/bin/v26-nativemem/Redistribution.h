#pragma once
#include <cstdint>
#include <stdexcept>
namespace by5v5 {
enum class InsertAction { Here, ConsumeHere, MoveLeft, MoveRight, Split };
struct Neighbor { bool exists=false; unsigned real=0,fictitious=0; bool structuralSafe=false; };
struct InsertPlan {
    InsertAction action=InsertAction::Split;
    unsigned consume=0;
    bool incomingGoesToNeighbor=false;
    unsigned incomingRank=0;
};
template<unsigned C=23> unsigned effective(unsigned real,unsigned fictitious) {
    if(real>C || fictitious>C-real) throw std::invalid_argument("occupancy");
    return real+fictitious;
}
template<unsigned C=23>
InsertPlan planInsert(unsigned real,unsigned fictitious,unsigned rank,const Neighbor& left,const Neighbor& right) {
    auto e=effective<C>(real,fictitious);
    if(rank>real) throw std::invalid_argument("rank");
    if(e<C) return {InsertAction::Here,0,false,rank};
    if(fictitious) return {InsertAction::ConsumeHere,1,false,rank};
    for(unsigned side=0;side<2;++side) {
        const auto& n=side?right:left;
        if(!n.exists) continue;
        auto ne=effective<C>(n.real,n.fictitious);
        if(!n.structuralSafe || (ne==C && !n.fictitious)) continue;
        unsigned consume=ne==C?1:0;
        if(!side) {
            bool incoming=rank==0;
            return {InsertAction::MoveLeft,consume,incoming,incoming?n.real:rank-1};
        }
        bool incoming=rank==real;
        return {InsertAction::MoveRight,consume,incoming,incoming?0:rank};
    }
    return {InsertAction::Split,0,false,rank};
}
struct FusionPlan { bool possible=false; unsigned left=0,right=0; };
template<unsigned C=23>
FusionPlan planFusion(unsigned lr,unsigned lf,unsigned rr,unsigned rf,bool sameParent,unsigned maxMerged=17,unsigned maxSource=6) {
    effective<C>(lr,lf);effective<C>(rr,rf);
    if(!maxMerged || maxMerged>C || maxSource>C) throw std::invalid_argument("fusion limits");
    if(!sameParent || !lr || !rr || lr+rr>maxMerged || (lr>maxSource && rr>maxSource)) return {};
    unsigned total=lr+lf+rr+rf,need=total>maxMerged?total-maxMerged:0,best=2*C+1;
    FusionPlan result;
    for(unsigned side=0;side<2;++side) {
        if((side?rr:lr)>maxSource) continue;
        unsigned a=0,b=0;
        if(!side) a=lr+lf>maxSource?lr+lf-maxSource:0;
        else b=rr+rf>maxSource?rr+rf-maxSource:0;
        unsigned remaining=need>a+b?need-a-b:0;
        auto da=remaining<lf-a?remaining:lf-a;a+=da;remaining-=da;
        auto db=remaining<rf-b?remaining:rf-b;b+=db;remaining-=db;
        if(!remaining && a+b<best) { result={true,a,b};best=a+b; }
    }
    return result;
}
}
