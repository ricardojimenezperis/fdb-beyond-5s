#pragma once
#if defined(__SSE2__) || defined(__AVX2__)
#include <immintrin.h>
#endif
#include <cstddef>
#include <cstdint>
#include <cstring>
namespace block_reader_lcp {
struct Result { int order; uint32_t lcp; };
// Compares exactly n readable bytes. The caller handles unequal key lengths.
// No load crosses the supplied extent. SIMD is selected by the build ISA flags.
// Input lengths are bounded by FDB StringRef (int); the 32-bit LCP is sufficient.
inline __attribute__((always_inline)) Result compare(const unsigned char* a,const unsigned char* b,size_t n) {
 size_t offset=0;
#if defined(__AVX2__)
 while(n-offset>=32){
  auto x=_mm256_loadu_si256(reinterpret_cast<const __m256i*>(a+offset));
  auto y=_mm256_loadu_si256(reinterpret_cast<const __m256i*>(b+offset));
  uint32_t different=~uint32_t(_mm256_movemask_epi8(_mm256_cmpeq_epi8(x,y)));
  if(different){size_t p=offset+__builtin_ctz(different);return {int(a[p])-int(b[p]),uint32_t(p)};}
  offset+=32;
 }
#endif
#if defined(__SSE2__)
 while(n-offset>=16){
  auto x=_mm_loadu_si128(reinterpret_cast<const __m128i*>(a+offset));
  auto y=_mm_loadu_si128(reinterpret_cast<const __m128i*>(b+offset));
  uint32_t different=(~uint32_t(_mm_movemask_epi8(_mm_cmpeq_epi8(x,y))))&0xffff;
  if(different){size_t p=offset+__builtin_ctz(different);return {int(a[p])-int(b[p]),uint32_t(p)};}
  offset+=16;
 }
#endif
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
 if(n-offset>=8){
  uint64_t x,y;std::memcpy(&x,a+offset,8);std::memcpy(&y,b+offset,8);
  uint64_t different=x^y;
  if(different){size_t p=offset+__builtin_ctzll(different)/8;return {int(a[p])-int(b[p]),uint32_t(p)};}
  offset+=8;
 }
#endif
 for(;offset<n;++offset)if(a[offset]!=b[offset])return {int(a[offset])-int(b[offset]),uint32_t(offset)};
 return {0,uint32_t(n)};
}
} // namespace block_reader_lcp
