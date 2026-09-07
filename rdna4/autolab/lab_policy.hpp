// SPDX-License-Identifier: MIT
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numeric>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>
#include "../layer_r4/head_contract.hpp"
namespace autolab {
inline void require(bool v,const char* s){if(!v)throw std::runtime_error(s);}
inline double median(std::vector<double> v){require(!v.empty(),"Empty timing data");for(double x:v)require(std::isfinite(x),"Nonfinite timing value");std::sort(v.begin(),v.end());return v.size()%2?v[v.size()/2]:(v[v.size()/2-1]+v[v.size()/2])/2;}
struct Decision{bool accepted=false;double saved_ms=0,lower_ms=0,upper_ms=0,relative=0;std::string reason;};
// Paired means preserve same-round A/B comparisons. This bootstrap is a screening
// heuristic, not a claim of IID timing, global optimality or game validation.
inline Decision screen(const std::vector<double>& base,const std::vector<double>& test,
                       bool numerically_equal,bool output_sensitive){
    Decision d;
    if(!numerically_equal){d.reason="numerical_difference";return d;}
    if(!output_sensitive){d.reason="output_sensitivity_unconfirmed";return d;}
    if(base.size()!=test.size() || base.size()<21){d.reason="insufficient_paired_samples";return d;}
    require(base.size()<=4096,"Timing sample cap");std::vector<double> delta;delta.reserve(base.size());
    for(size_t i=0;i<base.size();++i){require(std::isfinite(base[i])&&base[i]>0&&std::isfinite(test[i])&&test[i]>0,"Invalid GPU duration");delta.push_back(base[i]-test[i]);}
    d.saved_ms=std::accumulate(delta.begin(),delta.end(),0.0)/delta.size();
    d.relative=d.saved_ms/(std::accumulate(base.begin(),base.end(),0.0)/base.size());
    std::mt19937 rng(6009070);std::uniform_int_distribution<size_t> pick(0,delta.size()-1);
    std::vector<double> boot(2048);
    for(double& v:boot){v=0;for(size_t i=0;i<delta.size();++i)v+=delta[pick(rng)];v/=delta.size();}
    std::sort(boot.begin(),boot.end());d.lower_ms=boot[51];d.upper_ms=boot[1996];
    d.accepted=d.lower_ms>0 && d.relative>=0.01;
    d.reason=d.accepted?"candidate_passes_screen_not_release_approval":"no_reliable_one_percent_gain";
    return d;
}
struct LabSummary{size_t numerical_checks=0,policy_checks=0;};
inline LabSummary cpu_lab(){
    namespace h=head_contract;LabSummary out;
    // CPU models of the RECOVERED HEAD arithmetic/layout contract. This does not
    // execute HSACO, emulate the AMD ISA, implement Swin, or predict GPU time.
    // Use exactly representable finite FP8 inputs so 32-term products have no
    // binary32 reduction-order ambiguity within this deliberately bounded suite.
    std::mt19937 rng(619070);constexpr unsigned rows=16,cols=64;
    const unsigned char values[]={0,0x20,0x28,0x30,0x38,0xa0,0xa8,0xb0,0xb8};
    for(unsigned round=0;round<12;++round){
        std::vector<unsigned char> x(h::INPUT_BYTES),w(h::WEIGHT_BYTES);
        for(unsigned r=0;r<rows;++r)for(unsigned k=0;k<h::K;++k)x[h::tensor(r,k)]=values[rng()%9];
        for(unsigned k=0;k<h::K;++k)for(unsigned n=0;n<cols;++n)w[h::weight(k,n)]=values[rng()%9];
        std::vector<unsigned char> reference(rows*cols),candidate(rows*cols);
        for(unsigned r=0;r<rows;++r)for(unsigned n=0;n<cols;++n){
            float acc=0;
            for(unsigned k=0;k<h::K;k+=32){double chunk=0;for(unsigned j=0;j<32;++j)chunk+=double(h::decode8(x[h::tensor(r,k+j)]))*h::decode8(w[h::weight(k+j,n)]);acc=h::round16(float(chunk)+acc);}
            reference[r*cols+n]=h::encode8(acc);
        }
        for(unsigned tile:{16u,32u}){
            for(unsigned bn=0;bn<cols;bn+=tile)for(unsigned r=0;r<rows;++r){
                std::vector<float> accum(tile,0);
                for(unsigned k=0;k<h::K;k+=32)for(unsigned n=0;n<tile;++n){
                    float c=0;for(unsigned part=0;part<2;++part)for(unsigned j=0;j<16;++j){auto kk=k+part*16+j;c+=h::decode8(x[h::tensor(r,kk)])*h::decode8(w[h::weight(kk,bn+n)]);}
                    accum[n]=h::round16(c+accum[n]);
                }
                for(unsigned n=0;n<tile;++n)candidate[r*cols+bn+n]=h::encode8(accum[n]);
            }
            for(size_t i=0;i<reference.size();++i){require(reference[i]==candidate[i],"Portable Head contract model mismatch");++out.numerical_checks;}
        }
    }
    // A rounding-sensitive counterexample: never replace repeated FP16 rounding
    // with one final FP32-to-FP16 cast and call that an equivalent optimization.
    float repeated=64.0f;for(unsigned i=0;i<32;++i)repeated=h::round16(repeated+0.03125f);
    require(repeated!=h::round16(64.0f+32*0.03125f),"Rounding counterexample was lost");++out.numerical_checks;
    std::vector<double> a(32,24.0),b(32,23.0),slow(32,25.0),equal(32,24.0),tiny(32,23.999);
    require(screen(a,b,true,true).accepted,"Clean improvement rejected");++out.policy_checks;
    require(!screen(a,b,true,false).accepted,"Insensitive output accepted");++out.policy_checks;
    require(!screen(a,b,false,true).accepted,"Wrong result accepted");++out.policy_checks;
    require(!screen(a,slow,true,true).accepted,"Regression accepted");++out.policy_checks;
    require(!screen(a,equal,true,true).accepted,"No-op accepted");++out.policy_checks;
    require(!screen(a,tiny,true,true).accepted,"Negligible change accepted");++out.policy_checks;
    require(!screen({24,24},{23,23},true,true).accepted,"Tiny sample accepted");++out.policy_checks;
    for(size_t i=0;i<b.size();++i)b[i]=i%2?25:23;
    require(!screen(a,b,true,true).accepted,"Alternating noise accepted");++out.policy_checks;
    return out;
}
}
