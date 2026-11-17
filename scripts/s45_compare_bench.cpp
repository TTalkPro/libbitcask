// S45-6 基准：同一份源码分别链接 7.0.3 基线与 S45 树。只用公共 API。
// 不进 CMake（独立对比工具）。结果见 docs/design/s45-6-bench-zh.md。
// 构建（在某个 build 目录内，静态库已就绪）：
//   g++ -O2 -std=c++23 -flto=auto -I../third_party/unordered_dense/include \
//       -I../include -Igenerated ../scripts/s45_compare_bench.cpp -o s45bench \
//       libbitcask_cask.a libbitcask_merge.a libbitcask_hybrid.a \
//       libbitcask_text_plugin.a libbitcask_index_authority.a \
//       libbitcask_fileops.a libbitcask_docmap.a libbitcask_bm25.a \
//       libbitcask_text.a -licuuc -ldl -licudata libbitcask_vector_plugin.a \
//       libbitcask_vector.a libbitcask_format.a libbitcask_io.a \
//       libbitcask_simd.a -lz -ltbb -lpthread
// 运行：./s45bench [all|bulk|batch|flush|ckpt]
#include <bitcask/cask.hpp>
#include <bitcask/index_registry.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <random>
#include <string>
#include <thread>
#include <vector>
#include <future>
#include <unistd.h>
using namespace bitcask; using clk=std::chrono::steady_clock;
static std::span<const std::byte> B(std::string_view s){return {reinterpret_cast<const std::byte*>(s.data()),s.size()};}
static double ms_since(clk::time_point t){return std::chrono::duration<double,std::milli>(clk::now()-t).count();}
static std::vector<std::string> vocab;
static std::string mk_text(std::mt19937_64& r, int words){std::string s; for(int i=0;i<words;++i){ if(i) s+=" "; s+=vocab[r()%vocab.size()]; } return s;}
struct Env{ std::filesystem::path dir; oki::IndexRegistry reg; CaskOptions o; };
static CaskOptions opts(int dim){ CaskOptions o; o.read_write=true; o.enable_search=true; o.vector_dim=(uint16_t)dim; o.sync_every_n=0;
  search::SearchLayerConfig s; s.analyzer_config.type=text::AnalyzerType::Whitespace; o.search_config=s; return o; }
static std::filesystem::path fresh_dir(const char* tag){ auto d=std::filesystem::temp_directory_path()/("s45b-"+std::string(tag)+"-"+std::to_string(::getpid())); std::filesystem::remove_all(d); return d; }
static void put_one(Cask& c, std::mt19937_64& r, const std::string& k, int dim){ std::string t=mk_text(r,12); DocInput d; d.text=B(t); std::vector<float> v; if(dim){ v.resize(dim); std::uniform_real_distribution<float> u(-1,1); for(auto&x:v)x=u(r); d.vector=v; } (void)c.put_doc(B(k),d); }
int main(int argc,char**argv){
  setvbuf(stdout, nullptr, _IOLBF, 0);
  const std::string which = argc>1? argv[1] : "all";
  for(int i=0;i<5000;++i) vocab.push_back("w"+std::to_string(i));
  auto run_bulk=[&](const char* name,int writers,int per,int dim){
    auto dir=fresh_dir(name); oki::IndexRegistry reg; auto c=*Cask::open(dir.string(),opts(dim),&reg);
    auto t0=clk::now(); std::vector<std::thread> ws;
    for(int w=0;w<writers;++w) ws.emplace_back([&,w]{ std::mt19937_64 r(w+1); for(int i=0;i<per;++i) put_one(*c,r,"k"+std::to_string(w)+"_"+std::to_string(i),dim); });
    for(auto&t:ws)t.join(); double wms=ms_since(t0); c->flush_index(); double tms=ms_since(t0);
    std::printf("%-22s writers=%d docs=%d dim=%d  write=%.0f ms  end2end=%.0f ms  %.0f docs/s\n",name,writers,writers*per,dim,wms,tms,writers*per/(tms/1000));
    auto t1=clk::now(); c->close(); double cms=ms_since(t1);
    auto t2=clk::now(); { auto c2=*Cask::open(dir.string(),opts(dim),&reg); c2->flush_index(); std::printf("%-22s close=%.0f ms reopen=%.0f ms\n","",cms,ms_since(t2)); c2->close(); }
    std::filesystem::remove_all(dir); };
  if(which=="all"||which=="bulk"){
    run_bulk("bulk_1w_text",1,200000,0);
    run_bulk("bulk_4w_text",4,50000,0);
    run_bulk("bulk_8w_text",8,25000,0);
    run_bulk("bulk_4w_vec32",4,25000,32);
  }
  if(which=="all"||which=="batch"){
    auto dir=fresh_dir("batch"); oki::IndexRegistry reg; auto c=*Cask::open(dir.string(),opts(0),&reg);
    std::mt19937_64 r(9); std::vector<std::string> keys, vals; std::vector<Cask::BatchItem> items;
    const int N=10000, rounds=10; auto t0=clk::now();
    for(int k=0;k<rounds;++k){ keys.assign(N,""); vals.assign(N,""); items.resize(N);
      for(int i=0;i<N;++i){ keys[i]="b"+std::to_string(k)+"_"+std::to_string(i); vals[i]=mk_text(r,12); items[i]={B(keys[i]),B(vals[i])}; }
      (void)c->put_batch(items); }
    c->flush_index(); double ms=ms_since(t0);
    std::printf("%-22s docs=%d  end2end=%.0f ms  %.0f docs/s\n","put_batch_10k_x10",N*rounds,ms,N*rounds/(ms/1000)); c->close(); std::filesystem::remove_all(dir);
  }
  if(which=="all"||which=="flush"){
    auto dir=fresh_dir("flush"); oki::IndexRegistry reg; auto c=*Cask::open(dir.string(),opts(0),&reg);
    std::atomic<bool> stop{false}; std::vector<std::thread> ws;
    for(int w=0;w<4;++w) ws.emplace_back([&,w]{ std::mt19937_64 r(w+11); uint64_t i=0; while(!stop) put_one(*c,r,"f"+std::to_string(w)+"_"+std::to_string(i++%20000),0); });
    std::this_thread::sleep_for(std::chrono::milliseconds(300)); std::vector<double> lat;
    int starved=0;
    for(int p=0;p<20;++p){ auto t0=clk::now(); auto f=std::async(std::launch::async,[&]{ c->flush_index(); });
      if(f.wait_for(std::chrono::seconds(10))!=std::future_status::ready){ ++starved; stop=true; f.wait(); lat.push_back(ms_since(t0)); break; }
      lat.push_back(ms_since(t0)); std::this_thread::sleep_for(std::chrono::milliseconds(20)); }
    stop=true; for(auto&t:ws)t.join(); std::sort(lat.begin(),lat.end());
    if(starved) std::printf("%-22s 4 writers continuous: STARVED (>10 s until writers stopped; probes done=%zu)\n","flush_index_latency",lat.size());
    else std::printf("%-22s 4 writers continuous: p50=%.1f ms p90=%.1f ms max=%.1f ms\n","flush_index_latency",lat[10],lat[18],lat[19]);
    c->close(); std::filesystem::remove_all(dir);
  }
  if(which=="all"||which=="ckpt"){
    auto dir=fresh_dir("ckpt"); oki::IndexRegistry reg; auto c=*Cask::open(dir.string(),opts(0),&reg);
    { std::mt19937_64 r(3); for(int i=0;i<100000;++i) put_one(*c,r,"c"+std::to_string(i),0); c->flush_index(); }
    std::atomic<bool> stop{false}; std::atomic<uint64_t> ops{0}; std::vector<std::thread> ws;
    for(int w=0;w<2;++w) ws.emplace_back([&,w]{ std::mt19937_64 r(w+21); uint64_t i=0; while(!stop){ put_one(*c,r,"c"+std::to_string(i++%100000),0); ++ops; } });
    std::this_thread::sleep_for(std::chrono::milliseconds(200)); std::vector<double> lat;
    for(int p=0;p<5;++p){ auto t0=clk::now(); (void)c->checkpoint(); lat.push_back(ms_since(t0)); std::this_thread::sleep_for(std::chrono::milliseconds(100)); }
    stop=true; for(auto&t:ws)t.join(); std::sort(lat.begin(),lat.end());
    std::printf("%-22s 100k docs + 2 writers: median=%.0f ms max=%.0f ms (writes during=%llu)\n","checkpoint_latency",lat[2],lat[4],(unsigned long long)ops.load());
    c->close(); std::filesystem::remove_all(dir);
  }
}
