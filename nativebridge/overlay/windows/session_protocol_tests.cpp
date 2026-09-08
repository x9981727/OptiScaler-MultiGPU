// SPDX-License-Identifier: MIT
#include <iostream>
#include <stdexcept>
#include "session_protocol.hpp"
#include "test_support.hpp"

namespace {
void check(bool v,const char* why){if(!v)throw std::runtime_error(why);}
}
int main(){
    try{
        nb::ipc::Message msg{};
        nb::session::Hello request{};
        request.role=nb::session::Role::ConsumerRequest;
        request.processingAdapter={0x22334455u,1};
        check(nb::session::MakeHelloMessage(request,msg)==nb::session::Result::Ok,"request encode");
        nb::session::Hello parsed{};
        check(nb::session::ParseHelloMessage(msg,parsed)==nb::session::Result::Ok,"request decode");
        check(parsed.processingAdapter==request.processingAdapter,"processing adapter round trip");
        auto bad=msg;bad.payload[72]=std::byte{1};
        check(nb::session::ParseHelloMessage(bad,parsed)==nb::session::Result::Reserved,"hello reserved validation");

        nb::session::Hello accepted{};
        accepted.role=nb::session::Role::ProducerAccept;accepted.session=11;accepted.generation=4;accepted.viewport=7;
        accepted.renderAdapter={0x10203040u,2};accepted.processingAdapter=request.processingAdapter;
        accepted.extent={1920,1080};accepted.colorFormat=nb::Format::Rgba8;
        check(nb::session::MakeHelloMessage(accepted,msg)==nb::session::Result::Ok,"accept encode");
        check(nb::session::ParseHelloMessage(msg,parsed)==nb::session::Result::Ok,"accept decode");
        check(parsed.session==11&&parsed.generation==4&&parsed.extent==accepted.extent,"accept fields");

        nb::session::HandleSet handles{};handles.session=11;handles.generation=4;handles.renderAdapter=accepted.renderAdapter;
        handles.processingAdapter=accepted.processingAdapter;handles.extent=accepted.extent;handles.colorFormat=accepted.colorFormat;
        handles.heap=0x100;for(size_t i=0;i<nb::kSlotCount;++i){handles.ready[i]=0x200+i;handles.done[i]=0x300+i;}
        check(nb::session::MakeHandleMessage(handles,msg)==nb::session::Result::Ok,"handles encode");
        nb::session::HandleSet handles2{};check(nb::session::ParseHandleMessage(msg,handles2)==nb::session::Result::Ok,"handles decode");
        check(handles2.heap==handles.heap&&handles2.done[2]==handles.done[2],"handles fields");

        auto packet=test::packet(9);packet.key.session=11;packet.key.generation=4;packet.key.viewport=7;
        packet.color.key=packet.depth.key=packet.motion.key=packet.camera.key=packet.key;
        nb::Token token{2,17};
        check(nb::session::MakeFrameMessage(token,packet,msg)==nb::session::Result::Ok,"frame encode");
        nb::Token token2{};nb::Packet packet2{};
        check(nb::session::ParseFrameMessage(msg,token2,packet2)==nb::session::Result::Ok,"frame decode");
        check(token2==token&&packet2.key==packet.key,"frame identity");
        check(nb::session::MakeReleaseMessage(token,msg)==nb::session::Result::Ok,"release encode");
        check(nb::session::ParseReleaseMessage(msg,token2)==nb::session::Result::Ok&&token2==token,"release decode");
        std::cout<<"PASS session protocol: hello/handles/frame/release schemas and fail-closed reserved bytes\n";
        return 0;
    }catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}
}
