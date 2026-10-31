#pragma once
#include <cstdint>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace by5 {
class SyntheticProfiles {
    unsigned capacity_ = 0;
    int64_t window_ = 0;
    std::vector<uint32_t> offsets_, weights_;
    std::vector<int64_t> profiles_;
    uint64_t state_ = 0;

    template <class T> static T get(std::istream& in) {
        uint8_t b[sizeof(T)];
        in.read(reinterpret_cast<char*>(b), sizeof(T));
        if (!in) throw std::runtime_error("synthetic profiles truncated");
        uint64_t v = 0;
        for (size_t i = 0; i < sizeof(T); ++i) v |= uint64_t(b[i]) << (8 * i);
        return T(v);
    }

public:
    SyntheticProfiles(const std::string& path, uint64_t seed) : state_(seed) {
        std::ifstream in(path, std::ios::binary);
        char magic[8];
        in.read(magic, 8);
        if (!in || std::memcmp(magic, "BY5PROF1", 8)) throw std::runtime_error("not a synthetic profile file");
        capacity_ = get<uint32_t>(in);
        window_ = get<int64_t>(in);
        offsets_.resize(capacity_ + 1);
        weights_.resize(capacity_ + 1);
        for (auto& o : offsets_) o = get<uint32_t>(in);
        for (auto& w : weights_) w = get<uint32_t>(in);
        const uint64_t count = get<uint64_t>(in);
        if (count != offsets_.back()) throw std::runtime_error("synthetic profile count");
        profiles_.resize(count * capacity_);
        for (auto& v : profiles_) v = get<int64_t>(in);
        if (in.peek() != EOF) throw std::runtime_error("synthetic profile trailing data");
    }
    unsigned capacity() const { return capacity_; }
    int64_t window() const { return window_; }
    uint64_t next() {
        uint64_t z = (state_ += UINT64_C(0x9e3779b97f4a7c15));
        z = (z ^ (z >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
        z = (z ^ (z >> 27)) * UINT64_C(0x94d049bb133111eb);
        return z ^ (z >> 31);
    }
    uint64_t below(uint64_t n) {
        if (!n) throw std::invalid_argument("empty range");
        const uint64_t threshold = -n % n;
        for (;;) {
            const uint64_t r = next();
            if (r >= threshold) return r % n;
        }
    }
    const int64_t* sample(unsigned size) {
        if (size < 1 || size > capacity_) throw std::invalid_argument("occupancy");
        const uint32_t start = offsets_[size - 1], count = offsets_[size] - start;
        return &profiles_[(start + below(count)) * capacity_];
    }
};
}
