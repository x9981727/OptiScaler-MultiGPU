// SPDX-License-Identifier: MIT
#include <iostream>
#include <stdexcept>
#include "native_bridge/contract.hpp"
#include "test_support.hpp"
namespace { void check(bool v,const char* why){if(!v)throw std::runtime_error(why);} }
int main(){
    try{
        const nb::Extent e{1920,1080};
        const auto layout=nb::make_layout(e,nb::Format::Rgba16F);
        check(layout.has_value(),"fp16 layout");
        check(layout->planes[0].rowBytes==1920u*8u,"fp16 row bytes");
        check(layout->planes[1].rowBytes==1920u*4u,"depth row bytes");
        check(layout->planes[2].rowBytes==1920u*4u,"motion row bytes");
        check(layout->planes[0].format==nb::Format::Rgba16F,"fp16 layout format");
        auto p=test::packet(5);p.color.format=nb::Format::Rgba16F;
        check(nb::validate(p,test::policy(),p.timestampNs)==nb::Error::None,"fp16 packet validation");
        std::cout<<"PASS FP16 HDR contract: validation and 8-byte color transport layout\n";
        return 0;
    }catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}
}
