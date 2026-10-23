#include "hud.h"
#include "sprites.h"
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <vector>
#include <fstream>

extern "C" {
void js_begin(int,int,double);
const char* js_add_bar(double,double,double);
int js_frame(double,const std::uint8_t*);
const char* js_analyze(double,const char*);
const char* js_manual(int,double,const char*);
}

// Independent implementation of Java's RNG, used only to generate test footage.
struct ReferenceRandom {
    std::uint64_t seed;
    explicit ReferenceRandom(int tick) {
        const auto input=std::int32_t(std::uint32_t(tick)*312871u);
        seed=(std::uint64_t(std::int64_t(input))^0x5deece66dULL)&((1ULL<<48)-1);
    }
    int draw(int n) {
        for(;;){
            seed=(seed*0x5deece66dULL+11)&((1ULL<<48)-1);
            auto bits=seed>>17;
            if((n&-n)==n)return int((n*bits)>>31);
            int v=int(bits%n);
            if(std::int64_t(bits)-v+n-1<=2147483647)return v;
        }
    }
};

constexpr int W=640,H=360,S=2,X=330,Y=290;
void sprite(std::vector<std::uint8_t>& rgba,const jolt::sprites::Pixels& p,int x,int y){
    for(int cy=0;cy<9;++cy)for(int cx=0;cx<9;++cx){
        const int at=(cy*9+cx)*4;
        if(!p[at+3])continue;
        for(int dy=0;dy<S;++dy)for(int dx=0;dx<S;++dx){
            const int dest=((y+cy*S+dy)*W+x+cx*S+dx)*4;
            for(int c=0;c<3;++c)rgba[dest+c]=p[at+2-c];
        }
    }
}
std::vector<std::uint8_t> picture(int frame,bool cut){
    std::vector<std::uint8_t> rgba(W*H*4);
    for(int i=0;i<W*H;++i){rgba[i*4]=65;rgba[i*4+1]=95;rgba[i*4+2]=75;rgba[i*4+3]=255;}
    int tick=6100+frame/3+(cut&&frame>=900?305:0);
    ReferenceRandom random(tick);
    for(int slot=0;slot<10;++slot){
        const int offset=tick%61==0?random.draw(3)-1:0;
        const int x=X+(9-slot)*8*S,y=Y+offset*S;
        sprite(rgba,jolt::sprites::FOOD_EMPTY,x,y);sprite(rgba,jolt::sprites::FOOD_FULL,x,y);
        sprite(rgba,jolt::sprites::HEART_CONTAINER,X-101*S+(9-slot)*8*S,Y);
        sprite(rgba,jolt::sprites::HEART_FULL,X-101*S+(9-slot)*8*S,Y);
    }
    return rgba;
}

int main(){
    const std::string manual=js_manual(0,1,"@0.1 20 +--+000-++ @3.1667 20 -0+++0+--+");
    if(manual.find("\"tick\":6222")==std::string::npos)throw std::runtime_error(manual);
    const jolt::Bar bar{X,Y,S};const auto food=bar.rect(),heart=bar.hearts().rect();
    for(bool cut:{false,true}){
        js_begin(W,H,60);if(std::string(js_add_bar(X,Y,S))!="true")return 1;
        std::ofstream raw;
        if(cut&&std::getenv("JOLT_WRITE_FIXTURE"))raw.open("build/fixture.rgba",std::ios::binary);
        for(int frame=0;frame<1800;++frame){
            auto rgba=picture(frame,cut);
            if(raw)raw.write(reinterpret_cast<const char*>(rgba.data()),rgba.size());
            std::vector<std::uint8_t> strips;
            for(const auto r:{food,heart})for(int y=0;y<r.height;++y){
                const auto first=rgba.begin()+((r.top+y)*W+r.left)*4;
                strips.insert(strips.end(),first,first+r.width*4);
            }
            if(!js_frame(frame/60.,strips.data()))throw std::runtime_error("frame ingestion failed");
        }
        const std::string result=js_analyze(1,cut?"known-cut":"continuous");
        if(result.find("\"tick\": null")!=std::string::npos)throw std::runtime_error("fixture produced unmatched jolts");
        if(result.find("\"runs\": [\n ]")!=std::string::npos||result.find("error")!=std::string::npos)throw std::runtime_error(result);
        if((result.find("\"kind\": \"removed\"")!=std::string::npos)!=cut)throw std::runtime_error("cut classification mismatch: "+result);
        std::ofstream("build/"+std::string(cut?"known-cut":"continuous")+".json")<<result;
        std::printf("%s fixture: recovered ticks and %s cut classification.\n",cut?"Cut":"Continuous",cut?"positive":"negative");
    }
    js_begin(W,H,60);if(std::string(js_add_bar(-50,5,2)).find("error")==std::string::npos)return 1;
    std::puts("Browser adapter validation and manual cracking checks pass.");
}
