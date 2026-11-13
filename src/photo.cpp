// Gitmo - photos for SMS: JPEG encode / decode (Windows Imaging Component), the player's
// Steam screenshots for SnowRunner, and a worker thread that turns a captured game frame or
// a screenshot into a JPEG small enough to send (longest side kSendSide) plus a preview.
#include "gitmo.h"
#include <wincodec.h>
#include <shlwapi.h>
#include <shlobj.h>
#include <knownfolders.h>
#include <algorithm>
#include <thread>
#include <condition_variable>
#include <deque>
#include <set>
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")

namespace {
constexpr int kSendSide = 1280;      // photos are sent at most this big (longest side)
constexpr float kQuality = 0.82f;    // JPEG quality: ~150-300 KB for a game frame
constexpr int kPreviewSide = 720;    // the picture shown before sending

IWICImagingFactory* Wic() {
    thread_local IWICImagingFactory* f = nullptr;
    if (!f) {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f));
    }
    return f;
}
template <class T> void Rel(T*& p) { if (p) { p->Release(); p = nullptr; } }

void Fit(int w, int h, int maxSide, int& ow, int& oh) {
    float k = std::min(1.0f, (float)maxSide / (float)std::max(w, h));
    ow = std::max(1, (int)(w * k + 0.5f)); oh = std::max(1, (int)(h * k + 0.5f));
}

// any WIC source -> RGBA pixels at most maxSide
bool ToRgba(IWICBitmapSource* src, int maxSide, PhotoImg& out) {
    IWICImagingFactory* f = Wic();
    UINT w = 0, h = 0;
    if (!f || FAILED(src->GetSize(&w, &h)) || !w || !h) return false;
    int ow, oh; Fit((int)w, (int)h, maxSide, ow, oh);
    IWICBitmapScaler* sc = nullptr; IWICFormatConverter* cv = nullptr;
    bool ok = false;
    if (SUCCEEDED(f->CreateBitmapScaler(&sc)) && SUCCEEDED(sc->Initialize(src, ow, oh, WICBitmapInterpolationModeFant)) &&
        SUCCEEDED(f->CreateFormatConverter(&cv)) &&
        SUCCEEDED(cv->Initialize(sc, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))) {
        out.w = ow; out.h = oh; out.rgba.resize((size_t)ow * oh * 4);
        ok = SUCCEEDED(cv->CopyPixels(nullptr, ow * 4, (UINT)out.rgba.size(), out.rgba.data()));
        for (size_t i = 3; ok && i < out.rgba.size(); i += 4) out.rgba[i] = 255;   // photos are opaque
    }
    Rel(cv); Rel(sc);
    return ok;
}

bool EncodeSource(IWICBitmapSource* src, int maxSide, float quality, std::string& jpeg) {
    IWICImagingFactory* f = Wic();
    UINT w = 0, h = 0;
    if (!f || FAILED(src->GetSize(&w, &h)) || !w || !h) return false;
    int ow, oh; Fit((int)w, (int)h, maxSide, ow, oh);
    IWICBitmapScaler* sc = nullptr; IWICFormatConverter* cv = nullptr; IStream* st = nullptr;
    IWICBitmapEncoder* enc = nullptr; IWICBitmapFrameEncode* fr = nullptr; IPropertyBag2* bag = nullptr;
    bool ok = SUCCEEDED(f->CreateBitmapScaler(&sc)) && SUCCEEDED(sc->Initialize(src, ow, oh, WICBitmapInterpolationModeFant)) &&
              SUCCEEDED(f->CreateFormatConverter(&cv)) &&
              SUCCEEDED(cv->Initialize(sc, GUID_WICPixelFormat24bppBGR, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)) &&
              SUCCEEDED(CreateStreamOnHGlobal(nullptr, TRUE, &st)) &&
              SUCCEEDED(f->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, &enc)) &&
              SUCCEEDED(enc->Initialize(st, WICBitmapEncoderNoCache)) &&
              SUCCEEDED(enc->CreateNewFrame(&fr, &bag));
    if (ok) {
        PROPBAG2 opt{}; opt.pstrName = (LPOLESTR)L"ImageQuality";
        VARIANT v; VariantInit(&v); v.vt = VT_R4; v.fltVal = quality;
        bag->Write(1, &opt, &v);
        WICPixelFormatGUID fmt = GUID_WICPixelFormat24bppBGR;
        ok = SUCCEEDED(fr->Initialize(bag)) && SUCCEEDED(fr->SetSize(ow, oh)) && SUCCEEDED(fr->SetPixelFormat(&fmt)) &&
             IsEqualGUID(fmt, GUID_WICPixelFormat24bppBGR) && SUCCEEDED(fr->WriteSource(cv, nullptr)) &&
             SUCCEEDED(fr->Commit()) && SUCCEEDED(enc->Commit());
    }
    if (ok) {
        STATSTG stat{};
        LARGE_INTEGER zero{};
        ok = SUCCEEDED(st->Stat(&stat, STATFLAG_NONAME)) && SUCCEEDED(st->Seek(zero, STREAM_SEEK_SET, nullptr));
        if (ok) {
            jpeg.resize((size_t)stat.cbSize.QuadPart);
            ULONG got = 0;
            ok = SUCCEEDED(st->Read(jpeg.data(), (ULONG)jpeg.size(), &got)) && got == jpeg.size();
        }
    }
    Rel(bag); Rel(fr); Rel(enc); Rel(st); Rel(cv); Rel(sc);
    return ok;
}

// ---- the worker: one job at a time, newest wins ---------------------------------------------
struct Job { bool capture = false; PhotoImg frame; std::wstring path; };
std::mutex g_jobMx;
std::condition_variable g_jobCv;
std::deque<Job> g_jobs;
std::atomic<int> g_state{0};          // 0 none, 1 working, 2 ready, 3 failed
std::string g_resultJpeg;
PhotoImg g_resultPreview;
bool g_workerStarted;

void Worker() {
    for (;;) {
        Job j;
        {
            std::unique_lock<std::mutex> lk(g_jobMx);
            g_jobCv.wait(lk, [] { return !g_jobs.empty(); });
            j = std::move(g_jobs.back());
            g_jobs.clear();
        }
        std::string jpeg; PhotoImg preview;
        bool ok = false;
        IWICImagingFactory* f = Wic();
        if (f && j.capture && j.frame.w > 0) {
            // like a phone's camera roll: the picture is kept whether or not it's sent - in the
            // player's Steam screenshots when the game runs on Steam, otherwise in Pictures\Gitmo
            if (g_cfg.savePhotos && !(Steam_Available() && Steam_SaveScreenshot(j.frame))) Photo_SaveOwn(j.frame);
            IWICBitmap* bmp = nullptr;
            if (SUCCEEDED(f->CreateBitmapFromMemory(j.frame.w, j.frame.h, GUID_WICPixelFormat32bppRGBA, j.frame.w * 4,
                                                    (UINT)j.frame.rgba.size(), j.frame.rgba.data(), &bmp))) {
                ok = EncodeSource(bmp, kSendSide, kQuality, jpeg) && ToRgba(bmp, kPreviewSide, preview);
                bmp->Release();
            }
        } else if (f && !j.path.empty()) {
            std::string file = Photo_ReadFile(j.path);
            IStream* st = file.empty() ? nullptr : SHCreateMemStream((const BYTE*)file.data(), (UINT)file.size());
            IWICBitmapDecoder* dec = nullptr; IWICBitmapFrameDecode* fr = nullptr;
            if (st && SUCCEEDED(f->CreateDecoderFromStream(st, nullptr, WICDecodeMetadataCacheOnDemand, &dec)) && SUCCEEDED(dec->GetFrame(0, &fr)))
                ok = EncodeSource(fr, kSendSide, kQuality, jpeg) && ToRgba(fr, kPreviewSide, preview);
            Rel(fr); Rel(dec); Rel(st);
        }
        {
            std::lock_guard<std::mutex> lk(g_jobMx);
            if (!g_jobs.empty()) continue;   // a newer request came in meanwhile: that one counts
            g_resultJpeg.swap(jpeg); g_resultPreview = std::move(preview);
            g_state = ok && !g_resultJpeg.empty() && g_resultJpeg.size() <= sms::kMaxPhoto ? 2 : 3;
        }
        Log("photo: %s (%zu KB)", g_state == 2 ? "ready to send" : "couldn't be made", g_resultJpeg.size() / 1024);
    }
}
void Queue(Job&& j) {
    std::lock_guard<std::mutex> lk(g_jobMx);
    if (!g_workerStarted) { g_workerStarted = true; std::thread(Worker).detach(); }
    g_jobs.push_back(std::move(j));
    g_state = 1;
    g_jobCv.notify_one();
}
}   // namespace

std::string Photo_ReadFile(const std::wstring& path) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return {};
    LARGE_INTEGER sz{};
    std::string s;
    if (GetFileSizeEx(h, &sz) && sz.QuadPart > 0 && sz.QuadPart < 64ll * 1024 * 1024) {
        s.resize((size_t)sz.QuadPart);
        DWORD got = 0;
        if (!ReadFile(h, s.data(), (DWORD)s.size(), &got, nullptr) || got != s.size()) s.clear();
    }
    CloseHandle(h);
    return s;
}

bool Photo_Decode(const std::string& bytes, int maxSide, PhotoImg& out) {
    IWICImagingFactory* f = Wic();
    if (!f || bytes.empty()) return false;
    IStream* st = SHCreateMemStream((const BYTE*)bytes.data(), (UINT)bytes.size());
    IWICBitmapDecoder* dec = nullptr; IWICBitmapFrameDecode* fr = nullptr;
    bool ok = st && SUCCEEDED(f->CreateDecoderFromStream(st, nullptr, WICDecodeMetadataCacheOnDemand, &dec)) &&
              SUCCEEDED(dec->GetFrame(0, &fr)) && ToRgba(fr, maxSide, out);
    Rel(fr); Rel(dec); Rel(st);
    return ok;
}

bool Photo_EncodeJpeg(const PhotoImg& img, int maxSide, std::string& jpeg) {
    IWICImagingFactory* f = Wic();
    IWICBitmap* bmp = nullptr;
    if (!f || img.w <= 0 || FAILED(f->CreateBitmapFromMemory(img.w, img.h, GUID_WICPixelFormat32bppRGBA, img.w * 4,
                                                            (UINT)img.rgba.size(), (BYTE*)img.rgba.data(), &bmp))) return false;
    bool ok = EncodeSource(bmp, maxSide, kQuality, jpeg);
    bmp->Release();
    return ok;
}

// ---- photos kept without Steam (Epic and other stores): Pictures\Gitmo ------------------------
// Laid out like Steam's screenshot folder (thumbnails\ beside the pictures, same names), so the
// phone's photo panel lists either the same way.
std::wstring Photo_OwnFolder() {
#ifdef GITMO_TEST
    return ModuleDir() + L"\\Gitmo photos";   // the harness never writes to the real Pictures folder
#else
    std::wstring d;
    PWSTR p = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Pictures, KF_FLAG_CREATE, nullptr, &p)) && p) d = std::wstring(p) + L"\\Gitmo";
    CoTaskMemFree(p);
    if (d.empty()) d = ModuleDir() + L"\\Gitmo photos";
    return d;
#endif
}
static bool WriteWholeFile(const std::wstring& path, const std::string& data) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD w = 0;
    bool ok = WriteFile(h, data.data(), (DWORD)data.size(), &w, nullptr) && w == data.size();
    CloseHandle(h);
    if (!ok) DeleteFileW(path.c_str());
    return ok;
}
bool Photo_SaveOwn(const PhotoImg& img) {
    IWICImagingFactory* f = Wic();
    IWICBitmap* bmp = nullptr;
    if (!f || img.w <= 0 || FAILED(f->CreateBitmapFromMemory(img.w, img.h, GUID_WICPixelFormat32bppRGBA, img.w * 4,
                                                            (UINT)img.rgba.size(), (BYTE*)img.rgba.data(), &bmp))) return false;
    std::string full, thumb;
    bool ok = EncodeSource(bmp, 1 << 16, 0.92f, full) && EncodeSource(bmp, 240, kQuality, thumb);
    bmp->Release();
    std::wstring dir = Photo_OwnFolder();
    CreateDirectoryW(dir.c_str(), nullptr);
    CreateDirectoryW((dir + L"\\thumbnails").c_str(), nullptr);
    SYSTEMTIME t; GetLocalTime(&t);
    wchar_t base[64];
    swprintf_s(base, L"SnowRunner_%04u%02u%02u_%02u%02u%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    std::wstring name;
    for (int n = 1; ok && n < 100; n++) {   // two in the same second get _2, _3...
        name = std::wstring(base) + (n > 1 ? L"_" + std::to_wstring(n) : L"") + L".jpg";
        if (GetFileAttributesW((dir + L"\\" + name).c_str()) == INVALID_FILE_ATTRIBUTES) break;
    }
    ok = ok && WriteWholeFile(dir + L"\\" + name, full);
    if (ok) WriteWholeFile(dir + L"\\thumbnails\\" + name, thumb);
    char u[MAX_PATH * 3] = {};
    WideCharToMultiByte(CP_UTF8, 0, (dir + L"\\" + name).c_str(), -1, u, sizeof u, nullptr, nullptr);
    if (ok) Log("photo: saved %s (%dx%d, no Steam)", u, img.w, img.h);
    else Log("photo: couldn't save the photo in %s", u);
    return ok;
}

void Photo_FromCapture(PhotoImg&& frame) { Job j; j.capture = true; j.frame = std::move(frame); Queue(std::move(j)); }
void Photo_FromFile(const std::wstring& path) { Job j; j.path = path; Queue(std::move(j)); }
void Photo_Failed() { g_state = 3; }
int Photo_State() { return g_state; }
bool Photo_Take(std::string& jpeg, PhotoImg& preview) {
    std::lock_guard<std::mutex> lk(g_jobMx);
    if (g_state != 2) return false;
    jpeg.swap(g_resultJpeg); preview = std::move(g_resultPreview);
    g_resultJpeg.clear(); g_resultPreview = PhotoImg{};
    g_state = 0;
    return true;
}
void Photo_Cancel() { std::lock_guard<std::mutex> lk(g_jobMx); g_jobs.clear(); g_resultJpeg.clear(); g_resultPreview = PhotoImg{}; g_state = 0; }

// ---- decoding pictures for the phone (thumbnails, photos in a chat), on their own thread ------
namespace {
struct Dec { std::string key; std::wstring path; int maxSide; };
struct Done { std::string key; bool ok; PhotoImg img; };
std::mutex g_decMx;
std::condition_variable g_decCv;
std::deque<Dec> g_decQ;
std::deque<Done> g_decDone;
std::set<std::string> g_decPending;
bool g_decStarted;
void DecWorker() {
    for (;;) {
        Dec d;
        {
            std::unique_lock<std::mutex> lk(g_decMx);
            g_decCv.wait(lk, [] { return !g_decQ.empty(); });
            d = std::move(g_decQ.front()); g_decQ.pop_front();
        }
        Done r; r.key = d.key;
        r.ok = Photo_Decode(Photo_ReadFile(d.path), d.maxSide, r.img);
        std::lock_guard<std::mutex> lk(g_decMx);
        g_decPending.erase(d.key);
        g_decDone.push_back(std::move(r));
    }
}
}   // namespace
void Photo_DecodeAsync(const std::string& key, const std::wstring& path, int maxSide) {
    std::lock_guard<std::mutex> lk(g_decMx);
    if (g_decPending.count(key)) return;
    if (!g_decStarted) { g_decStarted = true; std::thread(DecWorker).detach(); }
    g_decPending.insert(key);
    g_decQ.push_back({ key, path, maxSide });
    g_decCv.notify_one();
}
bool Photo_DecodedNext(std::string& key, bool& ok, PhotoImg& img) {
    std::lock_guard<std::mutex> lk(g_decMx);
    if (g_decDone.empty()) return false;
    key = std::move(g_decDone.front().key); ok = g_decDone.front().ok; img = std::move(g_decDone.front().img);
    g_decDone.pop_front();
    return true;
}

// The player's Steam screenshots for SnowRunner (F12 in game), newest first:
//   <Steam>\userdata\<account id>\760\remote\1465360\screenshots\*.jpg  (+ thumbnails\ with the same names)
std::vector<std::wstring> Photo_SteamScreenshots(uint64_t steamId, int max) {
    std::wstring dir;
#ifdef GITMO_TEST
    { std::wstring t = ModuleDir() + L"\\testshots"; if (!getenv("GITMO_TEST_OWNPHOTOS") && GetFileAttributesW(t.c_str()) != INVALID_FILE_ATTRIBUTES) dir = t; }
#endif
    // without Steam (Epic and other stores): the photos the phone kept itself
    if (dir.empty() && !(steamId && Steam_Available())) dir = Photo_OwnFolder();
    if (dir.empty()) {
        wchar_t sp[MAX_PATH] = {}; DWORD cb = sizeof sp;
        if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath", RRF_RT_REG_SZ, nullptr, sp, &cb) != ERROR_SUCCESS || !steamId) return {};
        for (wchar_t* q = sp; *q; q++) if (*q == L'/') *q = L'\\';
        wchar_t b[MAX_PATH * 2];
        swprintf_s(b, L"%s\\userdata\\%u\\760\\remote\\1465360\\screenshots", sp, (unsigned)(steamId & 0xFFFFFFFFull));
        dir = b;
    }
    struct F { std::wstring p; FILETIME t; };
    std::vector<F> v;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*.jpg").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do { if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) v.push_back({ dir + L"\\" + fd.cFileName, fd.ftLastWriteTime }); } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    std::sort(v.begin(), v.end(), [](const F& a, const F& b) { return CompareFileTime(&a.t, &b.t) > 0; });
    std::vector<std::wstring> out;
    for (auto& f : v) { out.push_back(f.p); if ((int)out.size() >= max) break; }
    return out;
}
std::wstring Photo_SteamThumb(const std::wstring& shot) {
    size_t s = shot.find_last_of(L'\\');
    if (s == std::wstring::npos) return shot;
    std::wstring t = shot.substr(0, s) + L"\\thumbnails" + shot.substr(s);
    return GetFileAttributesW(t.c_str()) != INVALID_FILE_ATTRIBUTES ? t : shot;
}
