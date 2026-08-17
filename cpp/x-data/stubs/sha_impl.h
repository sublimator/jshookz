// sha_impl.h — Standalone SHA-256 and SHA-512 implementations for WASM.
//
// No external dependencies. Used by:
//   - xahaud backend (digest_stub.cpp wraps these as ripple::openssl_*_hasher)
//   - xdata backend (base58.cpp uses sha256_oneshot)
//
// Could be replaced with host-provided hardware-accelerated versions
// via function pointers for production.

#pragma once

#include <cstdint>
#include <cstring>

// ============================================================
// SHA-256 (FIPS 180-4)
// ============================================================

namespace sha_impl {

static const uint32_t sha256_k[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2,
};

#define RR32(x,n) (((x)>>(n))|((x)<<(32-(n))))
#define CH(x,y,z) (((x)&(y))^((~(x))&(z)))
#define MAJ(x,y,z) (((x)&(y))^((x)&(z))^((y)&(z)))
#define S256_0(x) (RR32(x,2)^RR32(x,13)^RR32(x,22))
#define S256_1(x) (RR32(x,6)^RR32(x,11)^RR32(x,25))
#define s256_0(x) (RR32(x,7)^RR32(x,18)^((x)>>3))
#define s256_1(x) (RR32(x,17)^RR32(x,19)^((x)>>10))

struct Sha256State {
    uint32_t h[8];
    uint8_t buf[64];
    uint32_t buf_len;
    uint64_t total;
};

inline void sha256_init(Sha256State& s) {
    s.h[0]=0x6a09e667; s.h[1]=0xbb67ae85; s.h[2]=0x3c6ef372; s.h[3]=0xa54ff53a;
    s.h[4]=0x510e527f; s.h[5]=0x9b05688c; s.h[6]=0x1f83d9ab; s.h[7]=0x5be0cd19;
    s.buf_len = 0; s.total = 0;
}

inline uint32_t be32(const uint8_t* p) {
    return (uint32_t)p[0]<<24|(uint32_t)p[1]<<16|(uint32_t)p[2]<<8|p[3];
}

inline void sha256_block(Sha256State& s, const uint8_t* blk) {
    uint32_t w[64], a,b,c,d,e,f,g,h;
    for (int i=0;i<16;i++) w[i]=be32(blk+i*4);
    for (int i=16;i<64;i++) w[i]=s256_1(w[i-2])+w[i-7]+s256_0(w[i-15])+w[i-16];
    a=s.h[0];b=s.h[1];c=s.h[2];d=s.h[3];e=s.h[4];f=s.h[5];g=s.h[6];h=s.h[7];
    for (int i=0;i<64;i++) {
        uint32_t t1=h+S256_1(e)+CH(e,f,g)+sha256_k[i]+w[i];
        uint32_t t2=S256_0(a)+MAJ(a,b,c);
        h=g;g=f;f=e;e=d+t1;d=c;c=b;b=a;a=t1+t2;
    }
    s.h[0]+=a;s.h[1]+=b;s.h[2]+=c;s.h[3]+=d;s.h[4]+=e;s.h[5]+=f;s.h[6]+=g;s.h[7]+=h;
}

inline void sha256_update(Sha256State& s, const void* data, size_t len) {
    auto* p = (const uint8_t*)data;
    s.total += len;
    if (s.buf_len > 0) {
        uint32_t fill = 64 - s.buf_len;
        if (len < fill) { std::memcpy(s.buf+s.buf_len, p, len); s.buf_len+=len; return; }
        std::memcpy(s.buf+s.buf_len, p, fill); sha256_block(s, s.buf); p+=fill; len-=fill; s.buf_len=0;
    }
    while (len >= 64) { sha256_block(s, p); p+=64; len-=64; }
    if (len > 0) { std::memcpy(s.buf, p, len); s.buf_len=len; }
}

inline void sha256_final(Sha256State& s, uint8_t out[32]) {
    uint64_t bits = s.total * 8;
    uint8_t pad = 1 + ((119 - s.buf_len) % 64);
    uint8_t zeros[72] = {0x80};
    sha256_update(s, zeros, pad);
    uint8_t len_be[8];
    for (int i=7;i>=0;i--) { len_be[i]=bits&0xff; bits>>=8; }
    sha256_update(s, len_be, 8);
    for (int i=0;i<8;i++) { out[i*4]=s.h[i]>>24; out[i*4+1]=s.h[i]>>16; out[i*4+2]=s.h[i]>>8; out[i*4+3]=s.h[i]; }
}

// ============================================================
// SHA-512 (FIPS 180-4)
// ============================================================

static const uint64_t sha512_k[80] = {
    0x428a2f98d728ae22ULL,0x7137449123ef65cdULL,0xb5c0fbcfec4d3b2fULL,0xe9b5dba58189dbbcULL,
    0x3956c25bf348b538ULL,0x59f111f1b605d019ULL,0x923f82a4af194f9bULL,0xab1c5ed5da6d8118ULL,
    0xd807aa98a3030242ULL,0x12835b0145706fbeULL,0x243185be4ee4b28cULL,0x550c7dc3d5ffb4e2ULL,
    0x72be5d74f27b896fULL,0x80deb1fe3b1696b1ULL,0x9bdc06a725c71235ULL,0xc19bf174cf692694ULL,
    0xe49b69c19ef14ad2ULL,0xefbe4786384f25e3ULL,0x0fc19dc68b8cd5b5ULL,0x240ca1cc77ac9c65ULL,
    0x2de92c6f592b0275ULL,0x4a7484aa6ea6e483ULL,0x5cb0a9dcbd41fbd4ULL,0x76f988da831153b5ULL,
    0x983e5152ee66dfabULL,0xa831c66d2db43210ULL,0xb00327c898fb213fULL,0xbf597fc7beef0ee4ULL,
    0xc6e00bf33da88fc2ULL,0xd5a79147930aa725ULL,0x06ca6351e003826fULL,0x142929670a0e6e70ULL,
    0x27b70a8546d22ffcULL,0x2e1b21385c26c926ULL,0x4d2c6dfc5ac42aedULL,0x53380d139d95b3dfULL,
    0x650a73548baf63deULL,0x766a0abb3c77b2a8ULL,0x81c2c92e47edaee6ULL,0x92722c851482353bULL,
    0xa2bfe8a14cf10364ULL,0xa81a664bbc423001ULL,0xc24b8b70d0f89791ULL,0xc76c51a30654be30ULL,
    0xd192e819d6ef5218ULL,0xd69906245565a910ULL,0xf40e35855771202aULL,0x106aa07032bbd1b8ULL,
    0x19a4c116b8d2d0c8ULL,0x1e376c085141ab53ULL,0x2748774cdf8eeb99ULL,0x34b0bcb5e19b48a8ULL,
    0x391c0cb3c5c95a63ULL,0x4ed8aa4ae3418acbULL,0x5b9cca4f7763e373ULL,0x682e6ff3d6b2b8a3ULL,
    0x748f82ee5defb2fcULL,0x78a5636f43172f60ULL,0x84c87814a1f0ab72ULL,0x8cc702081a6439ecULL,
    0x90befffa23631e28ULL,0xa4506cebde82bde9ULL,0xbef9a3f7b2c67915ULL,0xc67178f2e372532bULL,
    0xca273eceea26619cULL,0xd186b8c721c0c207ULL,0xeada7dd6cde0eb1eULL,0xf57d4f7fee6ed178ULL,
    0x06f067aa72176fbaULL,0x0a637dc5a2c898a6ULL,0x113f9804bef90daeULL,0x1b710b35131c471bULL,
    0x28db77f523047d84ULL,0x32caab7b40c72493ULL,0x3c9ebe0a15c9bebcULL,0x431d67c49c100d4cULL,
    0x4cc5d4becb3e42b6ULL,0x597f299cfc657e2aULL,0x5fcb6fab3ad6faecULL,0x6c44198c4a475817ULL,
};

#define RR64(x,n) (((x)>>(n))|((x)<<(64-(n))))
#define S512_0(x) (RR64(x,28)^RR64(x,34)^RR64(x,39))
#define S512_1(x) (RR64(x,14)^RR64(x,18)^RR64(x,41))
#define s512_0(x) (RR64(x,1)^RR64(x,8)^((x)>>7))
#define s512_1(x) (RR64(x,19)^RR64(x,61)^((x)>>6))
#define CH64(x,y,z) (((x)&(y))^((~(x))&(z)))
#define MAJ64(x,y,z) (((x)&(y))^((x)&(z))^((y)&(z)))

struct Sha512State {
    uint64_t h[8];
    uint8_t buf[128];
    uint32_t buf_len;
    uint64_t total;
};

inline void sha512_init(Sha512State& s) {
    s.h[0]=0x6a09e667f3bcc908ULL; s.h[1]=0xbb67ae8584caa73bULL;
    s.h[2]=0x3c6ef372fe94f82bULL; s.h[3]=0xa54ff53a5f1d36f1ULL;
    s.h[4]=0x510e527fade682d1ULL; s.h[5]=0x9b05688c2b3e6c1fULL;
    s.h[6]=0x1f83d9abfb41bd6bULL; s.h[7]=0x5be0cd19137e2179ULL;
    s.buf_len = 0; s.total = 0;
}

inline uint64_t be64(const uint8_t* p) {
    uint64_t v=0; for(int i=0;i<8;i++) v=(v<<8)|p[i]; return v;
}

inline void sha512_block(Sha512State& s, const uint8_t* blk) {
    uint64_t w[80], a,b,c,d,e,f,g,h;
    for (int i=0;i<16;i++) w[i]=be64(blk+i*8);
    for (int i=16;i<80;i++) w[i]=s512_1(w[i-2])+w[i-7]+s512_0(w[i-15])+w[i-16];
    a=s.h[0];b=s.h[1];c=s.h[2];d=s.h[3];e=s.h[4];f=s.h[5];g=s.h[6];h=s.h[7];
    for (int i=0;i<80;i++) {
        uint64_t t1=h+S512_1(e)+CH64(e,f,g)+sha512_k[i]+w[i];
        uint64_t t2=S512_0(a)+MAJ64(a,b,c);
        h=g;g=f;f=e;e=d+t1;d=c;c=b;b=a;a=t1+t2;
    }
    s.h[0]+=a;s.h[1]+=b;s.h[2]+=c;s.h[3]+=d;s.h[4]+=e;s.h[5]+=f;s.h[6]+=g;s.h[7]+=h;
}

inline void sha512_update(Sha512State& s, const void* data, size_t len) {
    auto* p = (const uint8_t*)data;
    s.total += len;
    if (s.buf_len > 0) {
        uint32_t fill = 128 - s.buf_len;
        if (len < fill) { std::memcpy(s.buf+s.buf_len, p, len); s.buf_len+=len; return; }
        std::memcpy(s.buf+s.buf_len, p, fill); sha512_block(s, s.buf); p+=fill; len-=fill; s.buf_len=0;
    }
    while (len >= 128) { sha512_block(s, p); p+=128; len-=128; }
    if (len > 0) { std::memcpy(s.buf, p, len); s.buf_len=len; }
}

inline void sha512_final(Sha512State& s, uint8_t out[64]) {
    uint64_t bits = s.total * 8;
    uint8_t pad = 1 + ((239 - s.buf_len) % 128);
    uint8_t zeros[144] = {0x80};
    sha512_update(s, zeros, pad);
    uint8_t len_be[16] = {};
    for (int i=15;i>=8;i--) { len_be[i]=bits&0xff; bits>>=8; }
    sha512_update(s, len_be, 16);
    for (int i=0;i<8;i++) {
        uint64_t v=s.h[i];
        out[i*8]=v>>56;out[i*8+1]=v>>48;out[i*8+2]=v>>40;out[i*8+3]=v>>32;
        out[i*8+4]=v>>24;out[i*8+5]=v>>16;out[i*8+6]=v>>8;out[i*8+7]=v;
    }
}

}  // namespace sha_impl

// One-shot SHA-256 declaration (defined in digest_stub.cpp)
extern "C" void sha256_oneshot(const uint8_t* in, size_t in_len, uint8_t* out);
