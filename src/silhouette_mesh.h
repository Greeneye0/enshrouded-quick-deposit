#pragma once
#include <array>
#include <vector>
#include <cstring>
#include <cmath>
#include <cstdint>
namespace silhouette {
struct Mesh { std::vector<std::array<float,3>> vertices; std::vector<uint16_t> indices; };
template<class T> T Read(const uint8_t* p) { T v; memcpy(&v,p,sizeof(v));return v; }
// Quantized PNTCT uses a planar 8-byte position stream. The two words store
// x in bits 11..31 of word 0, y across both words, and z in word 1 bits 0..20.
inline bool Decode(const std::vector<uint8_t>& data,const uint8_t* asset,const uint8_t* mesh,Mesh& out) {
    out={};
    uint32_t n=Read<uint32_t>(mesh+0x14),count=Read<uint32_t>(mesh+0x1c);
    uint64_t vo=uint64_t(Read<uint32_t>(asset+0x140))+Read<uint32_t>(mesh+0x28);
    uint64_t io=uint64_t(Read<uint32_t>(asset+0x130))+uint64_t(Read<uint32_t>(mesh+0x18))*2;
    if(mesh[0x34]!=4 || mesh[0x35]!=24 || mesh[0x37]!=1 || n<3 || n>20000 || !count || count>120000 || count%3 || vo+n*8>data.size() || io+count*2>data.size()) return false;
    uint64_t vb=Read<uint32_t>(asset+0x140),ib=Read<uint32_t>(asset+0x130);
    if(vo+n*8>vb+Read<uint32_t>(asset+0x144)||io+count*2>ib+Read<uint32_t>(asset+0x134))return false;
    float scale[3],offset[3];memcpy(scale,asset+0x40,12);memcpy(offset,asset+0x4c,12);
    for(int j=0;j<3;++j)if(!std::isfinite(scale[j])||scale[j]<=0||scale[j]>50||!std::isfinite(offset[j]))return false;
    out.vertices.reserve(n);
    for(uint32_t i=0;i<n;++i){
        uint32_t a=Read<uint32_t>(data.data()+vo+i*8),b=Read<uint32_t>(data.data()+vo+i*8+4);
        uint32_t q[3]={a>>11,((a<<10)|(b>>22))&0x1fffff,b&0x1fffff};
        std::array<float,3> v{};for(int j=0;j<3;++j)v[j]=offset[j]+scale[j]*(float(q[j])/2097151.f);out.vertices.push_back(v);
    }
    out.indices.resize(count);memcpy(out.indices.data(),data.data()+io,count*2);
    for(auto i:out.indices)if(i>=n){out={};return false;}
    return true;
}
}
