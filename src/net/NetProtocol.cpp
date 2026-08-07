#include "net/NetProtocol.h"
#include <cstring>
namespace rp::net {
namespace {
void put_u16(std::vector<uint8_t>& b, uint16_t v){ b.push_back(uint8_t(v)); b.push_back(uint8_t(v>>8)); }
void put_u32(std::vector<uint8_t>& b, uint32_t v){ for(int i=0;i<4;++i) b.push_back(uint8_t(v>>(8*i))); }
void put_u64(std::vector<uint8_t>& b, uint64_t v){ for(int i=0;i<8;++i) b.push_back(uint8_t(v>>(8*i))); }
bool get_u16(const uint8_t* p, size_t n, size_t& o, uint16_t& v){ if(o+2>n) return false; v=uint16_t(p[o])|uint16_t(p[o+1])<<8; o+=2; return true; }
bool get_u32(const uint8_t* p, size_t n, size_t& o, uint32_t& v){ if(o+4>n) return false; v=0; for(int i=0;i<4;++i) v|=uint32_t(p[o+i])<<(8*i); o+=4; return true; }
bool get_u64(const uint8_t* p, size_t n, size_t& o, uint64_t& v){ if(o+8>n) return false; v=0; for(int i=0;i<8;++i) v|=uint64_t(p[o+i])<<(8*i); o+=8; return true; }

void put_input(std::vector<uint8_t>& b, const rp_input_state& s){
    b.insert(b.end(), s.keys, s.keys + 256);          // keys are bytes: endian-neutral
    for (int i = 0; i < 8; ++i) put_u16(b, uint16_t(s.pad_axes[i]));  // int16 as u16 LE
    put_u16(b, s.pad_buttons);
}
bool get_input(const uint8_t* p, size_t n, size_t& o, rp_input_state& s){
    if (o + 256 > n) return false;
    std::memcpy(s.keys, p + o, 256); o += 256;
    for (int i = 0; i < 8; ++i){ uint16_t a; if(!get_u16(p,n,o,a)) return false; s.pad_axes[i]=int16_t(a); }
    uint16_t bt; if(!get_u16(p,n,o,bt)) return false; s.pad_buttons = bt;
    return true;
}
} // namespace

std::vector<uint8_t> encode_hello(const Hello& h){
    std::vector<uint8_t> b; b.push_back(uint8_t(MsgType::Hello));
    put_u32(b, h.abi_version);
    b.insert(b.end(), h.core_id, h.core_id + 64);
    put_u64(b, h.content_hash); put_u32(b, h.input_delay); put_u64(b, h.start_frame);
    return b;
}
std::vector<uint8_t> encode_state_sync(const StateSync& s){
    std::vector<uint8_t> b; b.push_back(uint8_t(MsgType::StateSync));
    put_u64(b, s.frame); put_u32(b, uint32_t(s.blob.size()));
    b.insert(b.end(), s.blob.begin(), s.blob.end());
    return b;
}
std::vector<uint8_t> encode_input(const Input& in){
    std::vector<uint8_t> b; b.push_back(uint8_t(MsgType::Input));
    put_u64(b, in.frame); b.push_back(in.port); put_input(b, in.state);
    return b;
}
std::vector<uint8_t> encode_checksum(const Checksum& c){
    std::vector<uint8_t> b; b.push_back(uint8_t(MsgType::Checksum));
    put_u64(b, c.frame); put_u32(b, c.crc);
    return b;
}
bool peek_type(const std::vector<uint8_t>& m, MsgType& out){
    if (m.empty()) return false; out = MsgType(m[0]); return true;
}
bool decode_hello(const std::vector<uint8_t>& m, Hello& h){
    if (m.empty() || m[0]!=uint8_t(MsgType::Hello)) return false;
    const uint8_t* p=m.data(); size_t n=m.size(), o=1;
    if(!get_u32(p,n,o,h.abi_version)) return false;
    if(o+64>n) return false; std::memcpy(h.core_id,p+o,64); h.core_id[63]='\0'; o+=64;
    return get_u64(p,n,o,h.content_hash) && get_u32(p,n,o,h.input_delay) && get_u64(p,n,o,h.start_frame);
}
bool decode_state_sync(const std::vector<uint8_t>& m, StateSync& s){
    if (m.empty() || m[0]!=uint8_t(MsgType::StateSync)) return false;
    const uint8_t* p=m.data(); size_t n=m.size(), o=1; uint32_t len;
    if(!get_u64(p,n,o,s.frame) || !get_u32(p,n,o,len)) return false;
    if(o+len>n) return false; s.blob.assign(p+o, p+o+len); return true;
}
bool decode_input(const std::vector<uint8_t>& m, Input& in){
    if (m.empty() || m[0]!=uint8_t(MsgType::Input)) return false;
    const uint8_t* p=m.data(); size_t n=m.size(), o=1;
    if(!get_u64(p,n,o,in.frame)) return false;
    if(o+1>n) return false; in.port=p[o]; o+=1;
    return get_input(p,n,o,in.state);
}
bool decode_checksum(const std::vector<uint8_t>& m, Checksum& c){
    if (m.empty() || m[0]!=uint8_t(MsgType::Checksum)) return false;
    const uint8_t* p=m.data(); size_t n=m.size(), o=1;
    return get_u64(p,n,o,c.frame) && get_u32(p,n,o,c.crc);
}
} // namespace rp::net
