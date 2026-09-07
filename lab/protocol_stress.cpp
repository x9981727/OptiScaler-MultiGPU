#include <ManagedXeFGProtocol.h>
#include <ManagedXeFGSequence.h>
#include <array>
#include <cstdint>
#include <iostream>
#include <limits>
#include <random>
#include <stdexcept>
#include <vector>
static void check(bool b,const char* s){if(!b)throw std::runtime_error(s);}
int main(){
 try {
  std::uint64_t attempts=0,completeCount=0,incompleteCount=0;
  // The fake API checks ID/order, NOT Intel's undocumented internal scheduling.
  for(unsigned seed=1;seed<=32;++seed){
   std::mt19937 rng(seed);std::uint32_t serial=0,expected=1;
   for(unsigned n=0;n<32768;++n){
    const bool valid=(rng()%5)>1;
    const auto id=MultiGPU::NextManagedXeFGId(serial,valid);
    unsigned sleep=0,native=0,fallback=0;std::vector<unsigned> phases;
    const std::int32_t status= n%7==0 ? static_cast<std::int32_t>(0x887A0005u) : n%3==0 ? 0x087A0001 : 0;
    const auto result=MultiGPU::RunManagedXeFGProtocol(valid,id,
     [&](auto x){++sleep;check(x==expected++,"sequence gap");return true;},
     [&](auto x,unsigned p){check(x==id,"marker ID changed");phases.push_back(p);return true;},
     [&](auto x){check(x==id,"tag ID changed");return true;},
     [&](){++native;return status;},[&](int){++fallback;++native;return status;});
    check(result==status,"native Present result lost");check(native==1,"original lost/duplicated");
    check(sleep==unsigned(valid)&&fallback==unsigned(!valid),"unexpected protocol/fallback");
    check(valid ? phases==std::vector<unsigned>({0,1,2,3,4,5}) : phases.empty(),"marker ordering");
    ++attempts;completeCount+=valid;incompleteCount+=!valid;
   }
  }
  for(unsigned missing : {0u,1u,103u,122u,231u,446u,464u,571u,1099u,2364u,1000000u}){
   std::uint32_t serial=72;
   for(unsigned k=0;k<missing;++k)check(MultiGPU::NextManagedXeFGId(serial,false)==0,"incomplete ID");
   check(MultiGPU::NextManagedXeFGId(serial,true)==73,"reactivation ID changed");
  }
  // An API failure invalidates that mock context; no assumption about later Intel recovery.
  for(unsigned fault=0;fault<9;++fault){
   unsigned native=0;const auto status=static_cast<std::int32_t>(0x887A0005u);
   auto result=MultiGPU::RunManagedXeFGProtocol(true,1,
    [&](auto){return fault!=0;},[&](auto,unsigned p){return fault!=p+1;},
    [&](auto){return fault!=7;},[&](){++native;return status;},[&](int){++native;return status;});
   check(native==1&&result==status,"fault path fabricated/lost Present");
  }
  bool ended=false;
  try {MultiGPU::RunManagedXeFGProtocol(true,1,[](auto){return true;},
   [&](auto,unsigned p){ended|=p==5;return true;},[](auto){return true;},
   []()->std::int32_t{throw std::runtime_error("injected");},[](int){return 0;});}
  catch(const std::runtime_error&){}
  check(ended,"unbalanced PRESENT markers after exception");
  for(unsigned mask=0;mask<16;++mask)
   check(MultiGPU::OwnsSecondaryXeLL(mask&1,mask&2,mask&4,mask&8)==(mask==15),"FSRFG scope leak");
  std::uint32_t max=(std::numeric_limits<std::uint32_t>::max)();
  check(MultiGPU::NextManagedXeFGId(max,true)==0,"ID wrap");
  std::cout<<"PASS production protocol: attempts="<<attempts<<" complete="<<completeCount
   <<" incomplete="<<incompleteCount<<" seeds=32; reactivation/fault/exception/16 backend scopes/wrap\n";
  std::cout<<"NOT A GPU BENCHMARK: mock API does not execute XeFG or validate flicker.\n";
  return 0;
 }catch(const std::exception& e){std::cerr<<"FAIL: "<<e.what()<<'\n';return 1;}
}
