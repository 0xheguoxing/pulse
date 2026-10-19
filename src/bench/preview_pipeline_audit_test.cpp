#include "../preview_host/gif_frames.h"
#include "../preview_host/folder_listing.h"
#include "../preview_host/preview_file_utils.h"
#include "../preview_host/preview_integrity.h"
#include <objbase.h>
#include <filesystem>
#include <fstream>
#include <cstdio>

namespace {
int failures=0;
void Check(bool ok,const char* label) { printf("[%s] %s\n",ok?"PASS":"FAIL",label); failures+=!ok; }
using Bytes=std::vector<uint8_t>;
Bytes Gif(int count) {
    Bytes b{'G','I','F','8','9','a',1,0,1,0,0x80,0,0,255,0,0,0,0,255};
    for(int i=0;i<count;++i) {
        const Bytes frame{0x21,0xf9,4,static_cast<uint8_t>((i%3+1)<<2),1,0,0,0,
            0x2c,0,0,0,0,1,0,1,0,0,2,2,static_cast<uint8_t>(i%2?0x4c:0x44),1,0};
        b.insert(b.end(),frame.begin(),frame.end());
    }
    b.push_back(0x3b); return b;
}
void Write(const std::filesystem::path& path,const Bytes& b) {
    std::ofstream file(path,std::ios::binary);
    file.write(reinterpret_cast<const char*>(b.data()),static_cast<std::streamsize>(b.size()));
}
}
int wmain() {
    using namespace pulse::preview;
    CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    const auto root=std::filesystem::absolute(std::filesystem::path(L"bench_data")/(L"pipeline-audit-"+std::to_wstring(GetCurrentProcessId())));
    std::filesystem::create_directories(root);
    for(DWORD attrs:{DWORD{0},DWORD{0x80000},DWORD{FILE_ATTRIBUTE_OFFLINE},DWORD{0x40000},DWORD{0x400000},DWORD{FILE_ATTRIBUTE_OFFLINE|0x80000},DWORD{0x480000},DWORD{0xc0000}})
        Check(IsOfflinePlaceholder(attrs)==((attrs&(FILE_ATTRIBUTE_OFFLINE|0x40000|0x400000))!=0),
            "OFFLINE and recall states override pin intent; local pinned files remain readable");
    Check(!IsOfflinePlaceholder(INVALID_FILE_ATTRIBUTES),
        "unknown attributes (a vanished path) are not an offline placeholder");
    const auto gif=root/L"animation.gif";
    Write(gif,Gif(100));
    Bytes pixels; UINT w=0,h=0,stride=0,sw=0,sh=0; uint32_t count=0,delay=0,loops=0; std::wstring error;
    bool frames=true;
    for(uint32_t i=0;i<100;++i) frames &= DecodeGifFrame(gif.wstring(),0,16,i,pixels,w,h,stride,count,delay,loops,sw,sh,&error) &&
        pixels.size()==4 && pixels[i%2?0:2]==255 && count==100;
    Check(frames,"GIF first sequential pass preserves all colors across disposal 1/2/3");
    for(uint32_t i:{3u,3u,0u,99u})
        Check(DecodeGifFrame(gif.wstring(),0,16,i,pixels,w,h,stride,count,delay,loops,sw,sh,&error) && pixels[i%2?0:2]==255,
            "GIF reverse/repeat/loop/random access keeps frame identity");
    Bytes huge=Gif(1); huge[6]=huge[8]=0xff; huge[7]=huge[9]=0x3f;
    const auto oversized=root/L"large.gif"; Write(oversized,huge);
    error.clear();
    Check(!DecodeGifFrame(oversized.wstring(),0,16,0,pixels,w,h,stride,count,delay,loops,sw,sh,&error) && error==L"gif-resource-limit",
        "GIF source canvas rejects excessive working memory before allocation");
    std::wstring listing;
    Check(MakeFolderListing(root.wstring(),0,false,listing) && listing.starts_with(L"PULSEARC\t1\tDIR\t-\t2\tscan-limit"),
        "folder deadline includes root probe and flat enumeration");
    const auto flat=root/L"flat"; std::filesystem::create_directory(flat);
    for(int i=0;i<4001;++i) { std::ofstream file(flat/(std::to_wstring(i)+L".txt")); }
    Check(MakeFolderListing(flat.wstring(),3000,true,listing),"enumerate isolated flat directory");
    DecodeResult result; result.kind=pulse::ipc::PreviewContentKind::Archive; result.text=listing;
    const auto integrity=DescribeIntegrity(result,true);
    Check(integrity.state==IntegrityState::Partial && integrity.loaded==4000 && integrity.total==4001,
        "display-only clipping reports 4000 loaded / 4001 total without counting summary as entry");
    for(int i=0;i<4001;++i) std::filesystem::remove(flat/(std::to_wstring(i)+L".txt"));
    std::filesystem::remove(flat); std::filesystem::remove(gif); std::filesystem::remove(oversized); std::filesystem::remove(root);
    CoUninitialize(); return failures?1:0;
}
