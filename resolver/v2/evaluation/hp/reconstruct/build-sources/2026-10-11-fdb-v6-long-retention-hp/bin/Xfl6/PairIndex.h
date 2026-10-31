#pragma once
#include <array>
#include <cstdint>
#include <limits>
#include <stdexcept>
namespace by5v5 {
constexpr std::uint32_t absent=std::numeric_limits<std::uint32_t>::max();
struct PairLink {
    std::uint32_t previous=absent,next=absent;
    std::uint8_t priority=0;
};
template<unsigned C=23> class PairIndex {
    static_assert(C<=31 && C>0);
    std::array<std::uint32_t,2*C+2> heads;
    std::uint64_t mask=0;
public:
    PairIndex() { heads.fill(absent); }
    std::uint32_t head(unsigned priority) const { return heads.at(priority); }
    std::uint32_t candidate() const {
        for(unsigned n=2*C+1;n;--n) if(mask&(std::uint64_t(1)<<n)) return heads[n];
        return absent;
    }
    template<class Lookup> void set(std::uint32_t leftId,unsigned priority,Lookup&& get) {
        if(leftId==absent || priority>2*C+1) throw std::invalid_argument("pair index");
        auto& x=get(leftId);unsigned old=x.priority;
        if(old>2*C+1) throw std::logic_error("priority");
        if(old==priority) return;
        if(old) {
            if(x.previous!=absent) get(x.previous).next=x.next;
            else {
                if(heads[old]!=leftId) throw std::logic_error("membership");
                heads[old]=x.next;
            }
            if(x.next!=absent)get(x.next).previous=x.previous;
            if(heads[old]==absent)mask&=~(std::uint64_t(1)<<old);
        }
        x.priority=static_cast<std::uint8_t>(priority);x.previous=x.next=absent;
        if(priority) {
            x.next=heads[priority];
            if(x.next!=absent)get(x.next).previous=leftId;
            heads[priority]=leftId;mask|=std::uint64_t(1)<<priority;
        }
    }
};
}
