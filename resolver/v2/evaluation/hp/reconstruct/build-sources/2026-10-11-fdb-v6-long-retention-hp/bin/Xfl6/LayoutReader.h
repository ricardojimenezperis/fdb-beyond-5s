#pragma once
#include <cstdint>
#include <fstream>
#include <iterator>
#include <numeric>
#include <stdexcept>
#include <string>
#include <vector>
namespace by5layout {
struct Layout {
    std::uint64_t seed, entries;
    std::uint32_t mode;
    std::vector<std::uint8_t> leaves;
    std::vector<std::vector<std::uint8_t>> levels;
};
inline Layout read(const std::string& path) {
    std::ifstream in(path,std::ios::binary);
    if (!in) throw std::runtime_error("cannot open layout");
    std::vector<unsigned char> b((std::istreambuf_iterator<char>(in)),{});
    if (b.size()<52 || std::string(b.begin(),b.begin()+8)!=std::string("BY5LAY2\0",8)) throw std::runtime_error("layout header");
    std::uint32_t crc=0xffffffffu;
    for (std::size_t i=0;i<b.size()-4;++i) {
        crc^=b[i];
        for (int bit=0;bit<8;++bit) crc=(crc>>1)^(0xedb88320u & (0u-(crc&1u)));
    }
    std::size_t p=8;
    auto take=[&](unsigned n) {
        if (p+n>b.size()) throw std::runtime_error("truncated layout");
        std::uint64_t v=0;
        for (unsigned i=0;i<n;++i) v|=std::uint64_t(b[p++])<<(8*i);
        return v;
    };
    auto version=take(4);
    Layout r;
    r.seed=take(8);r.mode=take(4);
    auto n=take(8);r.entries=take(8);auto d=take(4);auto ni=take(4);
    if (version!=2 || r.mode>1 || !n || n>b.size() || !d || d>64) throw std::runtime_error("invalid layout values");
    auto sizes=[&](std::uint64_t count) {
        if (count>b.size()-p || p+count>b.size()-4) throw std::runtime_error("truncated sizes");
        std::vector<std::uint8_t> v;
        v.reserve(count);
        for (std::uint64_t i=0;i<count;++i) {
            auto value=take(1);
            if (value<1 || value>23) throw std::runtime_error("invalid occupancy");
            v.push_back(value);
        }
        return v;
    };
    r.leaves=sizes(n);
    for (std::uint64_t i=0;i<d;++i) r.levels.push_back(sizes(take(4)));
    if (p!=b.size()-4 || take(4)!=(crc^0xffffffffu)) throw std::runtime_error("layout CRC or trailing bytes");
    if (r.levels[0].size()!=1 || std::accumulate(r.leaves.begin(),r.leaves.end(),UINT64_C(0))!=r.entries) throw std::runtime_error("root or entries");
    std::uint64_t found=0;
    for (std::size_t i=0;i<r.levels.size();++i) {
        auto& v=r.levels[i];found+=v.size();
        auto expected=i+1<r.levels.size()?r.levels[i+1].size():r.leaves.size();
        if (std::accumulate(v.begin(),v.end(),UINT64_C(0))!=expected) throw std::runtime_error("child count");
    }
    if (found!=ni) throw std::runtime_error("internal count");
    std::vector<unsigned char> child;
    child.reserve(r.leaves.size());
    for (auto n:r.leaves) child.push_back(n==23);
    for (auto it=r.levels.rbegin();it!=r.levels.rend();++it) {
        std::vector<unsigned char> parent;
        std::size_t at=0;
        for (auto fan:*it) {
            unsigned critical=0;
            for (unsigned j=0;j<fan;++j) critical+=child[at++];
            if (critical>23u-fan) throw std::runtime_error("insufficient internal slack");
            parent.push_back(critical==23u-fan);
        }
        child=std::move(parent);
    }
    return r;
}
}
