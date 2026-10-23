// Browser orchestration for Splosh Jolt. The pinned detector is in vendor/jolt.
#include "hud.h"
#include "timeline.h"
#include "manual.h"
#include "report.h"
#include <algorithm>
#include <cmath>
#include <deque>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>

namespace {
using namespace jolt;
std::string output;
std::vector<Frame> samples;
int width = 0, height = 0, chunk_frames = 90;
double fps = 60;
std::vector<Bar> bars;
std::vector<double> times;
struct Chunk {
    int start = 0, count = 0;
    std::vector<std::uint8_t> strips;
    BarModel::Rest rest;
};
struct RowState {
    BarModel model;
    int screen;
    std::vector<std::int8_t> states, levels;
    Chunk current;
    std::optional<Chunk> older, previous;
    RowState(Bar bar, Row kind, int screen) : model(bar, kind), screen(screen) {}
};
std::vector<std::unique_ptr<RowState>> rows;
std::string quote(const std::string& s) {
    std::string out = "\"";
    const char* hex = "0123456789abcdef";
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { out += '\\'; out += char(c); }
        else if (c < 32 || c == '<') { out += "\\u00"; out += hex[c>>4]; out += hex[c&15]; }
        else out += char(c);
    }
    return out+'"';
}
template <typename F> const char* respond(F f) {
    try { output = f(); }
    catch (const std::exception& e) { output = "{\"error\":"+quote(e.what())+"}"; }
    return output.c_str();
}
bool inside(const Rect& r) {
    return r.left >= 0 && r.top >= 0 && r.width > 0 && r.height > 0 &&
        r.left + r.width <= width && r.top + r.height <= height;
}
void settle(RowState& row, const Chunk& chunk, const BarModel::Rest* before, const BarModel::Rest* after) {
    const auto& rect = row.model.rect();
    const size_t bytes = size_t(rect.width)*rect.height*3;
    std::array<const BarModel::Rest*,3> rests{before,&chunk.rest,after};
    for (int frame=0; frame<chunk.count; ++frame) {
        const int at = chunk.start+frame;
        row.levels[at] = std::int8_t(row.model.measure(chunk.strips.data()+frame*bytes, rests,
                                                   row.states.data()+size_t(at)*SHANKS));
    }
}
void flush(RowState& row) {
    if (!row.current.count) return;
    row.current.rest = row.model.rest_of(row.current.strips.data(), row.current.count);
    if (row.previous) settle(row,*row.previous,row.older?&row.older->rest:nullptr,&row.current.rest);
    row.older = std::move(row.previous);
    if (row.older) std::vector<std::uint8_t>().swap(row.older->strips);
    row.previous = std::move(row.current);
    row.current = {};
}
int longest(double hours) {
    if (!std::isfinite(hours) || hours < .01 || hours > 72)
        throw std::invalid_argument("Longest game uptime must be between 0.01 and 72 hours.");
    return int(hours*3600*TICKS_PER_SECOND);
}
}

extern "C" {
const char* js_last() { return output.c_str(); }
void js_clear() {
    samples.clear(); rows.clear(); bars.clear(); times.clear(); output.clear();
}
int js_sample(int w, int h, const std::uint8_t* rgba) {
    if (w<=0 || h<=0 || size_t(w)*h>16000000 || samples.size()>=12) return 0;
    Frame f; f.width=w; f.height=h; f.pixels.resize(size_t(w)*h*3);
    for (size_t p=0; p<size_t(w)*h; ++p)
        for(int c=0;c<3;++c) f.pixels[p*3+c]=rgba[p*4+2-c];
    samples.push_back(std::move(f));
    return 1;
}
const char* js_locate() {
    return respond([] {
        const auto bar=locate(samples);
        samples.clear();
        if (!bar) return std::string("null");
        std::ostringstream out;
        out<<"{\"x\":"<<bar->x<<",\"y\":"<<bar->y<<",\"scale\":"<<bar->scale<<"}";
        return out.str();
    });
}
void js_begin(int w,int h,double rate) {
    js_clear(); width=w; height=h; fps=rate;
    chunk_frames=std::max(1,int(std::lround(fps*1.5)));
}
const char* js_add_bar(double x,double y,double scale) {
    return respond([&] {
        if (!std::isfinite(x)||!std::isfinite(y)||!std::isfinite(scale)||scale<.5||scale>24||bars.size()>=4)
            throw std::invalid_argument("Use valid x, y and scale values; up to four screens are supported.");
        const Bar bar{x,y,scale};
        if (!inside(bar.rect())) throw std::invalid_argument("The hunger bar extends outside the video. Adjust its position or scale.");
        const int screen=int(bars.size()); bars.push_back(bar);
        rows.push_back(std::make_unique<RowState>(bar,Row::Food,screen));
        if (inside(bar.hearts().rect())) rows.push_back(std::make_unique<RowState>(bar.hearts(),Row::Hearts,screen));
        return std::string("true");
    });
}
const char* js_rects() {
    return respond([] {
        std::ostringstream out; out<<'[';
        for(size_t i=0;i<rows.size();++i){
            const auto& r=rows[i]->model.rect();
            if(i)out<<',';
            out<<"{\"x\":"<<r.left<<",\"y\":"<<r.top<<",\"width\":"<<r.width<<",\"height\":"<<r.height<<'}';
        }
        out<<']'; return out.str();
    });
}
int js_frame(double time,const std::uint8_t* rgba) {
    try {
        if(rows.empty()||!std::isfinite(time)||(!times.empty()&&time<=times.back()))
            throw std::invalid_argument("Video frames must have increasing presentation timestamps.");
        const int at=int(times.size()); times.push_back(time);
        for(auto& pointer:rows){
            auto& row=*pointer; const auto& r=row.model.rect();
            const size_t pixels=size_t(r.width)*r.height;
            if(!row.current.count)row.current.start=at;
            const size_t start=row.current.strips.size();
            row.current.strips.resize(start+pixels*3);
            for(size_t p=0;p<pixels;++p)for(int c=0;c<3;++c)row.current.strips[start+p*3+c]=rgba[p*4+2-c];
            rgba+=pixels*4;
            row.states.resize(size_t(at+1)*SHANKS,UNREAD);row.levels.push_back(-1);
            if(++row.current.count>=chunk_frames)flush(row);
        }
        return 1;
    }catch(const std::exception& e){output="{\"error\":"+quote(e.what())+"}";return 0;}
}
const char* js_analyze(double hours,const char* title) {
    return respond([&] {
        const int max_tick=longest(hours);
        if(times.size()<2)throw std::invalid_argument("Fewer than two frames were decoded. Choose a longer video range.");
        Analysis analysis{bars,fps,times.front(),times.back(),0,{},{},{}};
        bool hearts=false; int chains=0;
        for(auto& pointer:rows){
            auto& row=*pointer;flush(row);
            if(row.previous)settle(row,*row.previous,row.older?&row.older->rest:nullptr,nullptr);
            const bool heart=row.model.row()==Row::Hearts;
            auto found=heart?find_heart_jolts(row.states,row.levels,times):find_jolts(row.states,row.levels,times);
            if(heart){
                auto faint=find_heart_chains(row.states,row.levels,times,found,chains);
                if(!faint.empty())chains=faint.back().chain+1;
                found.insert(found.end(),faint.begin(),faint.end());
            }
            hearts|=heart&&!found.empty();
            for(auto& j:found)j.screen=row.screen;
            analysis.jolts.insert(analysis.jolts.end(),found.begin(),found.end());
        }
        std::stable_sort(analysis.jolts.begin(),analysis.jolts.end(),[](const Jolt&a,const Jolt&b){return a.frame<b.frame;});
        size_t readable=0;
        for(size_t frame=0;frame<times.size();++frame){
            bool read=false;
            for(const auto& row:rows){
                if(row->model.row()!=Row::Food)continue;
                const auto first=row->states.begin()+frame*SHANKS;
                read|=std::any_of(first,first+SHANKS,[](auto s){return s!=UNREAD;});
            }
            readable+=read;
        }
        analysis.readable=double(readable)/times.size();
        std::vector<WobbleTable> tables;
        // Building tables is unnecessary if the HUD never produced a jolt.
        if(!analysis.jolts.empty()){
            for(int draws:SKIPPED_DRAWS)tables.emplace_back(Row::Food,max_tick,draws);
            if(hearts)for(int draws:HEARTS_ABOVE)tables.emplace_back(Row::Hearts,max_tick,draws);
        }
        const double tolerance=.06+2/fps;
        std::vector<std::optional<Fix>> fixes(analysis.jolts.size());
        for(int screen=0;screen<int(bars.size())&&!analysis.jolts.empty();++screen){
            std::vector<Jolt> own;std::vector<int> places;
            for(int i=0;i<int(analysis.jolts.size());++i)if(analysis.jolts[i].screen==screen){own.push_back(analysis.jolts[i]);places.push_back(i);}
            for(const auto& run:confirmed_runs(decode(own,tables,tolerance),max_tick,tolerance))
                for(auto fix:run){fix.jolt=places[fix.jolt];fixes[fix.jolt]=fix;}
        }
        analysis.runs=runs_in_order(fixes,analysis.jolts,tolerance);
        analysis.games=sort_into_games(analysis.runs,analysis.jolts,tolerance);
        std::string result=to_json(analysis,title,"local file");
        result.pop_back(); // final newline
        result.pop_back(); // final closing brace
        result+=",\"studio\":{\"frames\":"+std::to_string(times.size())+",\"maxHours\":"+std::to_string(hours)+
            ",\"description\":"+quote(describe(analysis))+"}}\n";
        return result;
    });
}
const char* js_manual(int heart,double hours,const char* text) {
    return respond([&]{
        const auto row=heart?Row::Hearts:Row::Food;
        const auto codes=parse_codes(row,text);
        const auto found=crack(row,codes,longest(hours));
        std::ostringstream out;out<<"{\"count\":"<<found.size()<<",\"text\":"<<quote(describe_cracks(row,codes,found,hours))<<",\"candidates\":[";
        for(size_t i=0;i<std::min(found.size(),size_t(500));++i){if(i)out<<',';out<<"{\"tick\":"<<found[i].tick<<",\"skippedDraws\":"<<found[i].skipped_draws<<'}';}
        out<<"]}";return out.str();
    });
}
const char* js_html(const char* json) { return respond([&]{return to_html(json);}); }
}
