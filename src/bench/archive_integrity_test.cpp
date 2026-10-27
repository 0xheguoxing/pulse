#include "../preview_host/archive_listing.h"
#include "preview_host_client.h"
#include <cstdio>
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace {
int failed = 0;
void Check(bool ok, const wchar_t* label) {
    std::wprintf(L"[%s] %s\n", ok ? L"PASS" : L"FAIL", label);
    if (!ok) ++failed;
}
bool Write(const std::wstring& path, const unsigned char* bytes, size_t size) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool ok = WriteFile(f, bytes, static_cast<DWORD>(size), &written, nullptr) && written == size;
    CloseHandle(f);
    return ok;
}
const unsigned char gz[] = {31,139,8,0,0,0,0,0,2,255,203,72,205,201,201,87,40,40,74,45,203,76,45,231,2,0,196,21,253,217,14,0,0,0};
const unsigned char xz[] = {253,55,122,88,90,0,0,4,230,214,180,70,2,0,33,1,22,0,0,0,116,47,229,163,1,0,13,104,101,108,108,111,32,112,114,101,118,105,101,119,10,0,0,0,90,191,69,89,152,228,23,124,0,1,38,14,8,27,224,4,31,182,243,125,1,0,0,0,0,4,89,90};
const unsigned char bz[] = {66,90,104,57,49,65,89,38,83,89,125,236,127,129,0,0,2,209,128,0,16,64,0,2,100,209,128,32,0,34,1,161,144,128,105,166,133,160,232,92,88,170,111,23,114,69,56,80,144,125,236,127,129};
const unsigned char tgz[] = {31,139,8,0,0,0,0,0,2,255,237,209,59,10,194,64,20,5,208,169,93,69,86,32,79,8,227,122,44,6,34,4,148,56,126,150,239,152,70,72,31,65,114,78,115,31,183,185,197,27,202,56,94,246,245,85,211,122,162,201,125,63,103,179,204,136,67,254,222,115,127,204,145,83,23,233,7,238,183,122,154,218,100,218,166,225,243,255,238,58,149,199,185,60,119,9,0,0,0,0,0,0,0,0,128,63,241,6,254,154,243,224,0,40,0,0};
const unsigned char txz[] = {253,55,122,88,90,0,0,4,230,214,180,70,2,0,33,1,22,0,0,0,116,47,229,163,224,39,255,0,104,93,0,52,25,73,238,141,240,186,200,255,155,255,242,12,105,175,17,235,99,84,137,29,247,43,190,83,128,248,176,109,36,108,25,112,161,52,4,133,198,148,212,255,92,134,251,27,195,240,244,247,86,86,228,88,50,140,3,94,94,75,22,0,202,250,77,139,240,75,137,4,27,189,74,104,81,106,152,95,108,201,104,245,8,244,179,159,210,117,73,79,33,239,62,6,247,189,235,66,41,233,193,68,211,182,0,0,107,150,235,107,98,139,228,66,0,1,132,1,128,80,0,0,40,51,241,250,177,196,103,251,2,0,0,0,0,4,89,90};
const unsigned char tbz[] = {66,90,104,57,49,65,89,38,83,89,34,99,153,240,0,0,113,251,128,202,144,4,0,64,1,101,128,0,128,98,100,223,192,8,8,32,0,84,52,163,212,104,208,201,181,25,12,143,40,36,160,134,154,50,0,104,7,220,196,106,16,88,244,33,18,164,224,58,48,173,2,24,30,52,161,55,137,204,17,157,228,50,56,232,102,156,56,238,18,232,111,219,77,106,44,2,91,213,102,86,120,146,17,16,31,139,185,34,156,40,72,17,49,204,248,0};

std::vector<unsigned char> Iso(bool udf) {
    std::vector<unsigned char> bytes(25 * 2048);
    auto both16 = [&](size_t p, unsigned v) {
        bytes[p] = static_cast<unsigned char>(v); bytes[p+1] = static_cast<unsigned char>(v >> 8);
        bytes[p+2] = bytes[p+1]; bytes[p+3] = bytes[p];
    };
    auto both32 = [&](size_t p, unsigned v) {
        for (unsigned n = 0; n < 4; ++n) { bytes[p+n] = static_cast<unsigned char>(v >> (n*8)); bytes[p+7-n] = bytes[p+n]; }
    };
    auto record = [&](size_t p, unsigned extent, unsigned size, const std::string& name, bool dir) {
        const size_t length = 33 + name.size() + (name.size() % 2 == 0 ? 1 : 0);
        bytes[p] = static_cast<unsigned char>(length); both32(p+2, extent); both32(p+10, size);
        bytes[p+18] = 126; bytes[p+19] = 1; bytes[p+20] = 1; bytes[p+25] = dir ? 2 : 0;
        both16(p+28, 1); bytes[p+32] = static_cast<unsigned char>(name.size());
        std::memcpy(bytes.data()+p+33, name.data(), name.size());
        return length;
    };
    const size_t p = 16 * 2048;
    bytes[p+881] = 1;
    bytes[p] = 1; std::memcpy(bytes.data()+p+1, "CD001", 5); bytes[p+6] = 1;
    both32(p+80, 25); both16(p+120, 1); both16(p+124, 1); both16(p+128, 2048);
    both32(p+132, 10);
    bytes[p+140] = 21; bytes[p+151] = 24;
    // Root-only path tables, little-endian at sector 21 and big-endian at 24.
    bytes[21*2048] = 1; bytes[21*2048+2] = 22; bytes[21*2048+6] = 1;
    bytes[24*2048] = 1; bytes[24*2048+5] = 22; bytes[24*2048+7] = 1;
    record(p+156, 22, 2048, std::string(1, '\0'), true);
    bytes[17*2048] = 255; std::memcpy(bytes.data()+17*2048+1,"CD001",5); bytes[17*2048+6] = 1;
    if (udf) {
        const char* ids[] = {"BEA01", "NSR03", "TEA01"};
        for (size_t n=0; n<3; ++n) { std::memcpy(bytes.data()+(18+n)*2048+1,ids[n],5); bytes[(18+n)*2048+6]=1; }
    }
    size_t r=22*2048;
    r+=record(r,22,2048,std::string(1,'\0'),true);
    r+=record(r,22,2048,std::string(1,'\1'),true);
    record(r,23,14,"HELLO.TXT;1",false);
    std::memcpy(bytes.data()+23*2048,"hello preview\n",14);
    return bytes;
}
}

int wmain() {
    const std::wstring root = L"bench_data\\archive_integrity_" + std::to_wstring(GetCurrentProcessId());
    std::filesystem::create_directories(root);
    // Expect the built-in readers, as on CI: an installed archive pack would answer
    // with 7-Zip instead. The preview host inherits this, so it sees no packs either.
    const std::wstring no_packs=std::filesystem::absolute(root+L"\\appdata").wstring();
    std::filesystem::create_directories(no_packs);
    SetEnvironmentVariableW(L"LOCALAPPDATA",no_packs.c_str());
    auto test = [&](const wchar_t* name, const unsigned char* data, size_t size, const wchar_t* format, const wchar_t* entry) {
        const std::wstring path=root+L"\\"+name;
        Check(Write(path,data,size),L"write isolated archive fixture");
        std::wstring text,error; uint32_t read=0;
        const bool ok=pulse::preview::MakeArchiveListing(path,text,read,&error);
        Check(ok && text.find(std::wstring(L"PULSEARC\t1\t")+format+L"\t-\t0\n")==0 && text.find(entry)!=std::wstring::npos,name);
        if (!ok) std::wprintf(L"error: %s\n",error.c_str());
    };
    test(L"hello.gz",gz,sizeof(gz),L"GZIP",L"\t14\t-\t-\thello\n");
    test(L"hello.xz",xz,sizeof(xz),L"XZ",L"\t14\t-\t-\thello\n");
    test(L"hello.bz2",bz,sizeof(bz),L"BZIP2",L"\t14\t-\t-\thello\n");
    test(L"archive.gz",tgz,sizeof(tgz),L"TAR.GZ",L"hello.txt");
    test(L"archive.xz",txz,sizeof(txz),L"TAR.XZ",L"hello.txt");
    test(L"archive.bz2",tbz,sizeof(tbz),L"TAR.BZ2",L"hello.txt");
    auto iso=Iso(false);
    test(L"plain.iso",iso.data(),iso.size(),L"ISO",L"HELLO.TXT");
    iso=Iso(true);
    const std::wstring path=root+L"\\bridge.iso";
    Check(Write(path,iso.data(),iso.size()),L"write UDF bridge fixture");
    std::wstring text,error; uint32_t read=0;
    Check(pulse::preview::MakeArchiveListing(path,text,read,&error) && text.find(L"PULSEARC\t1\tISO/UDF\t-\t1\n")==0 && text.find(L"HELLO.TXT")!=std::wstring::npos,L"UDF bridge explicitly reports incomplete ISO compatibility directory");
    // UDF-only recognition data must not fall through to libarchive's empty-TAR bidder.
    std::fill(iso.begin()+16*2048, iso.begin()+18*2048, static_cast<unsigned char>(0));
    const std::wstring udf_only=root+L"\\udf-only.iso";
    Check(Write(udf_only,iso.data(),iso.size()),L"write UDF-only recognition fixture");
    Check(!pulse::preview::MakeArchiveListing(udf_only,text,read,&error) && error==L"udf-invalid",
          L"UDF-only image cannot masquerade as an empty ISO or TAR tree");
    const std::wstring bad=root+L"\\bad.gz";
    Check(Write(bad,gz,10),L"write truncated gzip fixture");
    Check(!pulse::preview::MakeArchiveListing(bad,text,read,&error) && error.find(L"compressed-stream-")==0,L"truncated gzip returns explicit compressed stream failure");
    pulse_test::Host host;
    const bool started = host.Start();
    Check(started, L"start preview host for archive IPC warnings");
    if (started) {
        pulse_test::Result bridge_result, udf_result, stream_result;
        Check(host.Request(std::filesystem::absolute(path).wstring(), bridge_result) &&
              bridge_result.response.kind == pulse::ipc::PreviewContentKind::Archive &&
              bridge_result.text.starts_with(L"PULSEARC\t1\tISO/UDF\t-\t1\n") &&
              bridge_result.text.find(L"HELLO.TXT") != std::wstring::npos,
              L"IPC preserves incomplete ISO compatibility directory payload");
        Check(host.Request(std::filesystem::absolute(udf_only).wstring(), udf_result) &&
              udf_result.error == L"udf-invalid",
              L"IPC delivers UDF-only warning to UI after fallback");
        Check(host.Request(std::filesystem::absolute(bad).wstring(), stream_result) &&
              stream_result.error.starts_with(L"compressed-stream-"),
              L"IPC preserves compressed-stream failure through fallback");
    }
    host.Stop();
    std::filesystem::remove_all(root);
    return failed ? 1 : 0;
}
