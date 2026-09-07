// SPDX-License-Identifier: MIT
#pragma once
#include <array>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>
#include "deps/json.hpp"
extern "C" {
#include "deps/puff.h"
}
namespace r5 {
using J=nlohmann::json;using B=unsigned char;using U=std::uint32_t;using Q=std::uint64_t;
inline constexpr char PLAN_SHA[]="0867c68b824b3a6e4a8e7b1681efc80a1ab8251a50384bf005f600a80fe89a93";
inline constexpr char HEAD[]="_Z12k_final_head10HeadParams";
inline void need(bool p,const std::string& why){if(!p)throw std::runtime_error(why);}
inline Q integer(const J& v,Q maximum=Q(1)<<32){
    need(v.is_number_integer() && !v.is_boolean(),"Plan requires integer values");
    if(v.is_number_unsigned()){Q n=v.get<Q>();need(n<=maximum,"Plan integer exceeds limit");return n;}
    auto n=v.get<std::int64_t>();need(n>=0 && Q(n)<=maximum,"Plan integer outside limits");return Q(n);
}
template<class T>inline T le(const std::vector<B>& v,size_t at){
    need(at<=v.size() && sizeof(T)<=v.size()-at,"Binary field outside input");
    T n=0;for(size_t j=0;j<sizeof(T);++j)n|=T(v[at+j])<<(8*j);return n;
}
inline U crc32(const std::vector<B>& v){U c=~U(0);for(B b:v){c^=b;for(int k=0;k<8;++k)c=(c>>1)^(0xedb88320u&U(0-int(c&1)));}return ~c;}
// Read only one matching JSON entry, in memory. Never extract any path from an archive to disk.
inline std::vector<B> plan_bytes(const std::vector<B>& v){
    constexpr size_t cap=4*1024*1024;
    need(!v.empty() && v.size()<=32*1024*1024,"Plan/archive size limit");
    if(v.size()<4 || le<U>(v,0)!=0x04034b50u){need(v.size()<=cap,"JSON exceeds size cap");return v;}
    need(v.size()>=22,"Truncated ZIP");size_t eocd=SIZE_MAX;
    const size_t first=v.size()>65557?v.size()-65557:0;
    for(size_t p=v.size()-22;;--p){if(le<U>(v,p)==0x06054b50u && p+22+le<std::uint16_t>(v,p+20)==v.size()){eocd=p;break;}if(p==first)break;}
    need(eocd!=SIZE_MAX,"ZIP central directory absent");
    need(le<std::uint16_t>(v,eocd+4)==0 && le<std::uint16_t>(v,eocd+6)==0,"Split ZIP unsupported");
    size_t count=le<std::uint16_t>(v,eocd+10),same=le<std::uint16_t>(v,eocd+8),p=le<U>(v,eocd+16),central=le<U>(v,eocd+12);
    need(count>0 && count<4096 && same==count && p<=eocd && central<=eocd-p,"ZIP directory bounds");
    size_t end=p+central;std::vector<B> answer;bool found=false;
    for(size_t i=0;i<count;++i){
        need(p<=end && end-p>=46 && le<U>(v,p)==0x02014b50u,"Invalid ZIP directory entry");
        U flags=le<std::uint16_t>(v,p+8),method=le<std::uint16_t>(v,p+10),crc=le<U>(v,p+16);
        size_t packed=le<U>(v,p+20),unpacked=le<U>(v,p+24),nameSize=le<std::uint16_t>(v,p+28),extra=le<std::uint16_t>(v,p+30),comment=le<std::uint16_t>(v,p+32),local=le<U>(v,p+42);
        need(nameSize+extra+comment<=end-p-46,"ZIP name bounds");
        std::string name(reinterpret_cast<const char*>(v.data()+p+46),nameSize);
        std::replace(name.begin(),name.end(),'\\','/');
        bool match=name=="1080p.json" || name.ends_with("/1080p.json");
        if(match){
            need(!found,"More than one 1080p.json in selected ZIP");found=true;
            need(!(flags&1u) && (method==0 || method==8),"Encrypted/unsupported checkpoint ZIP");
            need(packed>0 && packed<=cap && unpacked>0 && unpacked<=cap,"Checkpoint entry exceeds cap");
            need(local<=v.size() && v.size()-local>=30 && le<U>(v,local)==0x04034b50u,"ZIP local header bounds");
            need(le<std::uint16_t>(v,local+8)==method && !(le<std::uint16_t>(v,local+6)&1u),"ZIP method mismatch");
            size_t data=local+30+le<std::uint16_t>(v,local+26)+le<std::uint16_t>(v,local+28);
            need(data<=v.size() && packed<=v.size()-data && data+packed<=eocd,"ZIP compressed range");
            answer.resize(unpacked);
            if(method==0){need(packed==unpacked,"Stored ZIP size mismatch");std::copy_n(v.begin()+data,unpacked,answer.begin());}
            else{unsigned long in=static_cast<unsigned long>(packed),out=static_cast<unsigned long>(unpacked);int rc=puff(answer.data(),&out,v.data()+data,&in);need(rc==0 && out==unpacked && in==packed,"Invalid raw-deflate checkpoint payload");}
            need(crc32(answer)==crc,"Checkpoint ZIP CRC32 mismatch");
        }
        p+=46+nameSize+extra+comment;
    }
    need(p==end && found,"Checkpoint has no unique plans/1080p.json");return answer;
}
inline std::vector<B> hex(const std::string& s){
    need(s.size()%2==0 && s.size()<=8192,"Invalid kernel argument byte count");
    auto nibble=[](char c)->U{if(c>='0' && c<='9')return U(c-'0');if(c>='a' && c<='f')return U(c-'a'+10);if(c>='A' && c<='F')return U(c-'A'+10);throw std::runtime_error("Invalid argument hex digit");};
    std::vector<B> out;out.reserve(s.size()/2);for(size_t i=0;i<s.size();i+=2)out.push_back(B(nibble(s[i])*16+nibble(s[i+1])));return out;
}
struct Ref{U allocation=0;size_t offset=0;};
struct Reloc{size_t at=0;Ref ref;};
struct Arg{std::vector<B> bytes;std::vector<Reloc> relocations;};
struct Cmd{std::string kernel;bool copy=false;std::array<U,3> grid{},block{};U shared=0;std::vector<Arg> args;Ref src,dst;size_t bytes=0;};
struct Plan{
    std::vector<size_t> allocations;std::vector<Cmd> commands;std::vector<size_t> heads;size_t total=0;
    Ref firstInput,lastObserved;J trace;
    void bounds(Ref r,size_t n,bool allowEnd=false)const{need(r.allocation<allocations.size(),"Unknown allocation ID");auto size=allocations[r.allocation];need(r.offset<=size && n<=size-r.offset && (allowEnd || r.offset<size),"Device reference exceeds planned allocation");}
    Ref ref(const J& v)const{Ref r{U(integer(v.at("allocation"),65535)),size_t(integer(v.at("offset"),Q(1)<<32))};bounds(r,0);return r;}
    static Ref pointer(const Cmd& c,size_t at){need(c.args.size()==1,"Expected one by-value argument");for(auto& r:c.args[0].relocations)if(r.at==at)return r.ref;throw std::runtime_error("Required original pointer relocation absent");}
    static std::array<U,3> dims(const J& a){need(a.is_array() && a.size()==3,"Expected three launch dimensions");std::array<U,3> d{};for(U j=0;j<3;++j){d[j]=U(integer(a[j],0x7fffffffu));need(d[j]>0,"Zero launch dimension");}return d;}
    static Plan parse(const J& root,bool fixed=true){
        Plan p;auto& alloc=root.at("allocations");need(alloc.is_array() && !alloc.empty() && alloc.size()<=128,"Allocation count invalid");
        p.allocations.resize(alloc.size());std::set<U> ids;
        for(auto& a:alloc){U id=U(integer(a.at("id"),127));size_t size=size_t(integer(a.at("size"),1024ULL*1024*1024));need(id<alloc.size() && size>0 && ids.insert(id).second,"Invalid allocation record");p.allocations[id]=size;p.total+=size;}
        need(p.total<=1024ULL*1024*1024,"Aggregate device allocation cap");
        size_t launches=0,copies=0;auto& events=root.at("events");need(events.is_array() && events.size()<=1024,"Event count invalid");
        J normalized=J::array();
        for(auto& e:events){
            std::string op=e.at("op").get<std::string>();if(op=="stage")continue;
            Cmd c;
            if(op=="memcpy"){
                c.copy=true;c.src=p.ref(e.at("src"));c.dst=p.ref(e.at("dst"));c.bytes=size_t(integer(e.at("size"),Q(1)<<30));need(c.bytes>0,"Zero device copy");p.bounds(c.src,c.bytes);p.bounds(c.dst,c.bytes);need(c.dst.allocation!=0,"Command writes model allocation");
                if(c.src.allocation==c.dst.allocation)need(c.src.offset+c.bytes<=c.dst.offset || c.dst.offset+c.bytes<=c.src.offset,"Overlapping device copy unsupported");
                ++copies;
            }else{
                need(op=="launch","Unknown original plan operation");c.kernel=e.at("kernel").get<std::string>();need(c.kernel.size()>2 && c.kernel.size()<256 && c.kernel.starts_with("_Z"),"Invalid original kernel symbol");
                c.grid=dims(e.at("grid"));c.block=dims(e.at("block"));need(Q(c.block[0])*c.block[1]*c.block[2]<=1024,"Block size exceeds limit");c.shared=U(integer(e.at("shared"),65536));
                need(e.at("args").is_array() && !e.at("args").empty() && e.at("args").size()<=32,"Argument count invalid");
                for(auto& a:e.at("args")){
                    Arg arg;arg.bytes=hex(a.at("bytes").get<std::string>());need(!arg.bytes.empty(),"Empty argument");std::set<size_t> occupied;
                    for(auto& r:a.at("relocations")){Reloc rel{size_t(integer(r.at("at"),4096)),p.ref(r)};need(rel.at<=arg.bytes.size() && 8<=arg.bytes.size()-rel.at,"Relocation exceeds argument");
                        for(size_t j=0;j<8;++j)need(occupied.insert(rel.at+j).second && arg.bytes[rel.at+j]==0,"Overlapping or unnormalized pointer relocation");arg.relocations.push_back(rel);}
                    c.args.push_back(std::move(arg));
                }
                if(c.kernel==HEAD){
                    need(c.args.size()==1 && c.args[0].bytes.size()==24 && c.args[0].relocations.size()==3,"Original Head ABI changed");
                    need(c.grid[1]==1 && c.grid[2]==1 && c.block==std::array<U,3>{256,1,1} && c.shared==0,"Original Head launch contract changed");
                    Ref in=pointer(c,0),out=pointer(c,8),weights=pointer(c,16);need(in.allocation!=0 && out.allocation!=0 && weights.allocation==0,"Head pointer roles unsupported");
                    p.bounds(in,size_t(c.grid[0])*8192);p.bounds(out,size_t(c.grid[0])*16384);p.bounds(weights,524288);
                    if(in.allocation==out.allocation)need(in.offset+size_t(c.grid[0])*8192<=out.offset || out.offset+size_t(c.grid[0])*16384<=in.offset,"In-place Head unsupported");
                    p.heads.push_back(p.commands.size());
                }
                ++launches;
            }
            p.commands.push_back(std::move(c));normalized.push_back(e);
        }
        need(!p.commands.empty() && !p.commands.front().copy && !p.commands.back().copy,"Boundary commands unsupported");
        p.firstInput=pointer(p.commands.front(),0);p.lastObserved=pointer(p.commands.back(),8);
        need(p.firstInput.allocation!=0 && p.lastObserved.allocation!=0,"Boundary tensor points to weights");
        if(fixed){
            need(p.allocations.size()==44 && p.total==806544188,"Not the recovered 1080p allocation contract");
            need(launches==154 && copies==4 && p.commands.size()==158 && p.heads.size()==1,"Not the recovered 154-launch/4-copy single-Head trace");
            need(p.commands.front().kernel.find("k_swin_var")!=std::string::npos && p.commands.back().kernel.find("k_swin_var")!=std::string::npos,"Unsupported boundary kernel family");
        }
        // This is normalized source data, not live user pointers or game surfaces.
        p.trace={{"allocations",alloc},{"events",normalized},{"first_input_ref",{{"allocation",p.firstInput.allocation},{"offset",p.firstInput.offset}}},{"last_observed_pointer_at8",{{"allocation",p.lastObserved.allocation},{"offset",p.lastObserved.offset}}}};
        return p;
    }
};
inline J fixture(){
    J rel=J::array({{{"at",0},{"allocation",1},{"offset",0}},{{"at",8},{"allocation",2},{"offset",0}},{{"at",16},{"allocation",0},{"offset",0}}});
    return {{"allocations",J::array({{{"id",0},{"size",32}},{{"id",1},{"size",32}},{{"id",2},{"size",32}}})},
        {"events",J::array({{{"op","launch"},{"kernel","_Z_fixture"},{"grid",{1,1,1}},{"block",{32,1,1}},{"shared",0},{"args",J::array({{{"bytes",std::string(48,'0')},{"relocations",rel}}})}}})}};
}
inline size_t self_test(){
    size_t tests=0;auto f=fixture();auto p=Plan::parse(f,false);need(p.total==96 && p.commands.size()==1 && p.firstInput.allocation==1,"Valid fixture rejected");++tests;
    auto rejects=[&](J bad){bool rejected=false;try{Plan::parse(bad,false);}catch(const std::exception&){rejected=true;}need(rejected,"Malformed plan accepted");++tests;};
    J bad=f;bad["allocations"][0]["size"]=-1;rejects(bad);
    bad=f;bad["allocations"][0]["id"]=1;rejects(bad);
    bad=f;bad["events"][0]["args"][0]["bytes"]=std::string(48,'1');rejects(bad);
    bad=f;bad["events"][0]["args"][0]["relocations"][0]["offset"]=33;rejects(bad);
    bad=f;bad["events"][0]["args"][0]["relocations"][0]["at"]=20;rejects(bad);
    bad=f;bad["events"][0]["block"]={1024,2,1};rejects(bad);
    bad=f;bad["events"][0]["grid"]={0,1,1};rejects(bad);
    bad=f;bad["events"][0]["op"]="system";rejects(bad);
    bad=f;bad["events"][0]["args"][0]["relocations"][1]["at"]=4;rejects(bad);
    auto copy=J{{"op","memcpy"},{"src",{{"allocation",1},{"offset",0}}},{"dst",{{"allocation",2},{"offset",0}}},{"size",32}};
    bad=f;bad["events"].insert(bad["events"].begin(),copy);rejects(bad);
    need(hex("0010abFF")==std::vector<B>({0,16,171,255}),"Hex conversion");++tests;
    need(crc32(std::vector<B>{'1','2','3','4','5','6','7','8','9'})==0xcbf43926,"ZIP CRC32 reference");++tests;
    return tests;
}
}
