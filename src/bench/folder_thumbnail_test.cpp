#include "../preview_host/folder_thumbnail.h"
#include <objidl.h>
#include <gdiplus.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <chrono>

namespace fs = std::filesystem;
namespace {
int failures = 0;
void Check(bool condition, const char* label) {
    std::cout << (condition ? "[PASS] " : "[FAIL] ") << label << '\n'; if (!condition) ++failures;
}
void Fixture(const fs::path& file, int variant) {
    constexpr int side = 160;
    BITMAPFILEHEADER header{}; header.bfType=0x4d42; header.bfOffBits=sizeof(header)+sizeof(BITMAPINFOHEADER);
    header.bfSize=header.bfOffBits+side*side*4;
    BITMAPINFOHEADER info{}; info.biSize=sizeof(info); info.biWidth=side; info.biHeight=-side;
    info.biPlanes=1; info.biBitCount=32; info.biCompression=BI_RGB;
    std::vector<uint8_t> bytes(side*side*4);
    for (int y=0; y<side; ++y) for (int x=0; x<side; ++x) {
        const size_t i=size_t(y*side+x)*4;
        const bool disk=(x-110)*(x-110)+(y-38)*(y-38)<225;
        bytes[i]=disk?60:static_cast<uint8_t>(variant==0?180-y/2:60+x/2);
        bytes[i+1]=disk?220:static_cast<uint8_t>(variant==1?170:80+y/2);
        bytes[i+2]=disk?250:static_cast<uint8_t>(variant==2?210:40+x/3);
        bytes[i+3]=255;
    }
    std::ofstream stream(file,std::ios::binary|std::ios::trunc);
    stream.write(reinterpret_cast<const char*>(&header),sizeof(header));
    stream.write(reinterpret_cast<const char*>(&info),sizeof(info));
    stream.write(reinterpret_cast<const char*>(bytes.data()),bytes.size());
}
bool SavePng(const fs::path& file,std::vector<uint8_t>& pixels,UINT size) {
    UINT count=0, bytes=0; Gdiplus::GetImageEncodersSize(&count,&bytes);
    std::vector<uint8_t> buffer(bytes); auto* codecs=reinterpret_cast<Gdiplus::ImageCodecInfo*>(buffer.data());
    Gdiplus::GetImageEncoders(count,bytes,codecs);
    Gdiplus::Bitmap image(static_cast<INT>(size),static_cast<INT>(size),static_cast<INT>(size*4),PixelFormat32bppPARGB,pixels.data());
    for (UINT i=0; i<count; ++i) if (wcscmp(codecs[i].MimeType,L"image/png")==0) return image.Save(file.c_str(),&codecs[i].Clsid)==Gdiplus::Ok;
    return false;
}
}
int wmain(int argc, wchar_t** argv) {
    CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    if (argc == 3 && (std::wstring(argv[1]) == L"--trace-once" || std::wstring(argv[1]) == L"--trace-force-once")) {
        const fs::path cache = fs::absolute(L"bench_data/folder_thumbnail_direct") /
            (L"trace_cache_" + std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(GetTickCount64()));
        SetEnvironmentVariableW(L"LOCALAPPDATA", cache.c_str());
        const bool force = std::wstring(argv[1]) == L"--trace-force-once";
        for (int run = 0; run < 2; ++run) {
            UINT w = 0, h = 0, stride = 0; std::vector<uint8_t> pixels;
            const auto begin = std::chrono::steady_clock::now();
            const bool ok = pulse::preview::DecodeFolderThumbnail(argv[2], 256, pixels, w, h, stride, false, force);
            const DWORD error = GetLastError();
            std::cout << "[TRACE] run=" << run << " ok=" << ok << " error=" << error << " ms="
                << std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count() << '\n';
            if (!ok) ++failures;
        }
        CoUninitialize(); return failures ? 1 : 0;
    }
    if (argc == 3 && std::wstring(argv[1]) == L"--bench") {
        const fs::path root = fs::absolute(L"bench_data/folder_thumbnail_direct/benchmark_cache");
        SetEnvironmentVariableW(L"LOCALAPPDATA", root.c_str());
        for (int mode = 0; mode < 3; ++mode) {
            if (mode == 2) SetEnvironmentVariableW(L"LOCALAPPDATA", nullptr);
            for (int run = 0; run < 10; ++run) {
                UINT w = 0, h = 0, stride = 0; std::vector<uint8_t> pixels;
                const auto begin = std::chrono::steady_clock::now();
                const bool ok = pulse::preview::DecodeFolderThumbnail(argv[2], 256, pixels, w, h, stride, false, mode != 1);
                std::cout << "[BENCH] " << (mode == 0 ? "force-cache" : mode == 1 ? "warm-cache" : "force-no-cache")
                    << " " << run << " " << std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count()
                    << " ms ok=" << ok << '\n';
                if (!ok) ++failures;
            }
        }
        CoUninitialize(); return failures ? 1 : 0;
    }
    Gdiplus::GdiplusStartupInput input; ULONG_PTR token=0; Gdiplus::GdiplusStartup(&token,&input,nullptr);
    const fs::path root=fs::absolute(L"bench_data/folder_thumbnail_direct");
    fs::create_directories(root/L"input"); fs::create_directories(root/L"empty");
    // Cache writes are confined to the generated fixture, never user preferences.
    wchar_t previous[32768]{}; GetEnvironmentVariableW(L"LOCALAPPDATA",previous,32768);
    SetEnvironmentVariableW(L"LOCALAPPDATA",(root/L"cache").c_str());
    for (int i=0; i<3; ++i) { std::error_code ec; fs::remove(root/L"input"/(std::to_wstring(i)+L".bmp"),ec); }
    UINT w=0,h=0,stride=0; std::vector<uint8_t> pixels, prior;
    const auto decode=[&](bool single=false,bool force=false) {return pulse::preview::DecodeFolderThumbnail((root/L"input").wstring(),256,pixels,w,h,stride,single,force);};
    Check(!pulse::preview::DecodeFolderThumbnail((root/L"empty").wstring(),256,pixels,w,h,stride),"empty uses ordinary folder icon");
    Check(!pulse::preview::DecodeFolderThumbnail(L"\\\\invalid\\share",256,pixels,w,h,stride),"UNC rejected before probing");
    for (int i=0; i<3; ++i) {
        Fixture(root/L"input"/(std::to_wstring(i)+L".bmp"),i);
        const auto begin=std::chrono::steady_clock::now();
        Check(decode(),"real WIC content renders");
        std::cout<<"[TIME] "<<i+1<<" cards "<<std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count()<<" ms\n";
        Check(w==256&&h==256&&stride==1024&&pixels.size()==256*256*4,"complete square PBGRA output");
        Check(pixels.size()==256*256*4&&pixels[3]==0&&pixels[(255*256+255)*4+3]==0,"transparent canvas corners");
        Check(prior.empty()||pixels!=prior,"adding an image changes the fan");
        Check(SavePng(root/(L"fan_"+std::to_wstring(i+1)+L".png"),pixels,256),"render fixture saved");
        prior=pixels;
    }
    auto begin=std::chrono::steady_clock::now(); Check(decode()&&pixels==prior,"warm cache exactly preserves pixels");
    std::cout<<"[TIME] warm "<<std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count()<<" ms\n";
    Check(decode(true)&&pixels!=prior,"single flag applies at high DPI");
    Check(SavePng(root/L"single.png",pixels,256),"single fixture saved");
    Fixture(root/L"input"/L"0.bmp",2);
    Check(decode()&&pixels!=prior,"child write invalidates persisted composite");
    prior=pixels; Check(decode(false,true)&&pixels==prior,"explicit refresh preserves deterministic rendering");
#ifdef PULSE_FOLDER_THUMBNAIL_TESTING
    fs::create_directories(root/L"budget");
    for (int i=0; i<3; ++i) Fixture(root/L"budget"/(std::to_wstring(i)+L".bmp"),i);
    const auto budget_decode=[&](bool force=false) {
        return pulse::preview::DecodeFolderThumbnail((root/L"budget").wstring(),256,pixels,w,h,stride,false,force);
    };
    pulse::preview::SetFolderThumbnailBudgetForTest(0);
    Check(!budget_decode(true) && GetLastError()==ERROR_TIMEOUT && pixels.empty() && !w,
        "enumeration budget exhaustion is transient and emits no bitmap");
    pulse::preview::SetFolderThumbnailBudgetForTest(450);
    Check(budget_decode(),"budget exhaustion did not persist a negative cache entry");
    prior=pixels;
    pulse::preview::SetFolderThumbnailBudgetForTest(450,1);
    Check(!budget_decode(true) && GetLastError()==ERROR_TIMEOUT && pixels.empty() && !w,
        "budget exhaustion after first card never emits partial success");
    pulse::preview::SetFolderThumbnailBudgetForTest(450);
    Check(budget_decode() && pixels==prior,"partial attempt preserves previously complete cache entry");
    SetLastError(ERROR_TIMEOUT);
    Check(!pulse::preview::DecodeFolderThumbnail((root/L"empty").wstring(),256,pixels,w,h,stride) &&
        GetLastError()!=ERROR_TIMEOUT,"ordinary empty result clears a previous timeout status");
#endif
    SetEnvironmentVariableW(L"LOCALAPPDATA",previous[0]?previous:nullptr);
    Gdiplus::GdiplusShutdown(token); CoUninitialize();
    std::cout<<"Failures: "<<failures<<'\n'; return failures?1:0;
}

