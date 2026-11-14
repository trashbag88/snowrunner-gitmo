// Gitmo - photos for SMS: JPEG encode / decode (Windows Imaging Component), the player's
// Steam screenshots for SnowRunner, and a worker thread that turns a captured game frame or
// a screenshot into a JPEG small enough to send (longest side kSendSide) plus a preview.
#include "gitmo.h"
#include "shared.h"
#include <wincodec.h>
#include <shlwapi.h>
#include <shlobj.h>
#include <knownfolders.h>
#include <algorithm>
#include <condition_variable>
#include <deque>
#include <set>
#include <climits>
#include <cmath>
#include <new>
#include <stdexcept>
#include <cerrno>
#include <process.h>
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")

namespace {
constexpr int kSendSide = 1280;      // photos are sent at most this big (longest side)
constexpr float kQuality = 0.82f;    // JPEG quality: ~150-300 KB for a game frame
constexpr int kPreviewSide = 720;    // the picture shown before sending
constexpr int kMaxImageSide = 65536;
constexpr uint64_t kMaxRgbaBytes = 256ull * 1024 * 1024;
constexpr uint64_t kMaxEncodedBytes = 256ull * 1024 * 1024;

struct WicContext {
    IWICImagingFactory* factory = nullptr;
    bool uninitialize = false;
    ~WicContext() {
        if (factory) factory->Release();
        if (uninitialize) CoUninitialize();
    }
};

IWICImagingFactory* Wic() {
    thread_local WicContext context;
    if (context.factory) return context.factory;
    HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(co) && co != RPC_E_CHANGED_MODE) {
        Log("photo: COM initialization failed (0x%08X)", (unsigned)co);
        return nullptr;
    }
    IWICImagingFactory* factory = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&factory));
    if (FAILED(hr) || !factory) {
        if (SUCCEEDED(co)) CoUninitialize();
        Log("photo: WIC initialization failed (0x%08X)", (unsigned)hr);
        return nullptr;
    }
    context.factory = factory;
    context.uninitialize = SUCCEEDED(co);
    return context.factory;
}
template <class T> void Rel(T*& p) { if (p) { p->Release(); p = nullptr; } }
template <class T> struct ComOwner {
    ComOwner() = default;
    ComOwner(const ComOwner&) = delete;
    ComOwner& operator=(const ComOwner&) = delete;
    T* p = nullptr;
    ~ComOwner() { Rel(p); }
    T** Put() { Rel(p); return &p; }
    T* Get() const { return p; }
    void Reset() { Rel(p); }
};

bool Fit(UINT w, UINT h, int maxSide, int& ow, int& oh) {
    if (!w || !h || w > kMaxImageSide || h > kMaxImageSide ||
        maxSide <= 0 || maxSide > kMaxImageSide) return false;
    float k = std::min(1.0f, (float)maxSide / (float)std::max(w, h));
    ow = std::max(1, (int)(w * k + 0.5f)); oh = std::max(1, (int)(h * k + 0.5f));
    uint64_t bytes = (uint64_t)ow * (uint64_t)oh * 4;
    return bytes <= kMaxRgbaBytes && bytes <= UINT_MAX;
}

bool ValidRgba(const PhotoImg& img, uint64_t& bytes) {
    if (img.w <= 0 || img.h <= 0 || img.w > kMaxImageSide || img.h > kMaxImageSide) return false;
    bytes = (uint64_t)img.w * (uint64_t)img.h * 4;
    return bytes <= kMaxRgbaBytes && bytes <= UINT_MAX && (uint64_t)img.w * 4 <= UINT_MAX &&
           img.rgba.size() == bytes;
}

bool CreateScaledConverter(IWICImagingFactory* f, IWICBitmapSource* src, int maxSide, const WICPixelFormatGUID& format,
                           IWICBitmapScaler*& sc, IWICFormatConverter*& cv, int& ow, int& oh) {
    UINT w = 0, h = 0;
    if (!src || !f || FAILED(src->GetSize(&w, &h)) || !Fit(w, h, maxSide, ow, oh)) return false;
    if (SUCCEEDED(f->CreateBitmapScaler(&sc)) &&
        SUCCEEDED(sc->Initialize(src, ow, oh, WICBitmapInterpolationModeFant)) &&
        SUCCEEDED(f->CreateFormatConverter(&cv)) &&
        SUCCEEDED(cv->Initialize(sc, format, WICBitmapDitherTypeNone, nullptr, 0,
                                WICBitmapPaletteTypeCustom))) return true;
    Rel(cv); Rel(sc);
    return false;
}

// any WIC source -> RGBA pixels at most maxSide
bool ToRgba(IWICBitmapSource* src, int maxSide, PhotoImg& out) {
    out = PhotoImg{};
    IWICImagingFactory* f = Wic();
    IWICBitmapScaler* sc = nullptr; IWICFormatConverter* cv = nullptr;
    int ow = 0, oh = 0;
    if (!CreateScaledConverter(f, src, maxSide, GUID_WICPixelFormat32bppRGBA, sc, cv, ow, oh)) return false;
    try {
        out.rgba.resize((size_t)ow * oh * 4);
    } catch (const std::bad_alloc&) {
        Rel(cv); Rel(sc);
        return false;
    } catch (const std::length_error&) {
        Rel(cv); Rel(sc);
        return false;
    }
    out.w = ow; out.h = oh;
    bool ok = SUCCEEDED(cv->CopyPixels(nullptr, ow * 4, (UINT)out.rgba.size(), out.rgba.data()));
    for (size_t i = 3; ok && i < out.rgba.size(); i += 4) out.rgba[i] = 255;   // photos are opaque
    Rel(cv); Rel(sc);
    if (!ok) out = PhotoImg{};
    return ok;
}

bool EncodeSource(IWICBitmapSource* src, int maxSide, float quality, std::string& jpeg) {
    jpeg.clear();
    IWICImagingFactory* f = Wic();
    IWICBitmapScaler* sc = nullptr; IWICFormatConverter* cv = nullptr; IStream* st = nullptr;
    IWICBitmapEncoder* enc = nullptr; IWICBitmapFrameEncode* fr = nullptr; IPropertyBag2* bag = nullptr;
    if (!f || !std::isfinite(quality) || quality < 0.0f || quality > 1.0f) return false;
    int ow = 0, oh = 0;
    bool ok = CreateScaledConverter(f, src, maxSide, GUID_WICPixelFormat24bppBGR, sc, cv, ow, oh) &&
              SUCCEEDED(CreateStreamOnHGlobal(nullptr, TRUE, &st)) &&
              SUCCEEDED(f->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, &enc)) &&
              SUCCEEDED(enc->Initialize(st, WICBitmapEncoderNoCache)) &&
              SUCCEEDED(enc->CreateNewFrame(&fr, &bag));
    if (ok) {
        PROPBAG2 opt{}; opt.pstrName = (LPOLESTR)L"ImageQuality";
        VARIANT v; VariantInit(&v); v.vt = VT_R4; v.fltVal = quality;
        ok = SUCCEEDED(bag->Write(1, &opt, &v));
        VariantClear(&v);
        WICPixelFormatGUID fmt = GUID_WICPixelFormat24bppBGR;
        ok = ok && SUCCEEDED(fr->Initialize(bag)) && SUCCEEDED(fr->SetSize(ow, oh)) && SUCCEEDED(fr->SetPixelFormat(&fmt)) &&
             IsEqualGUID(fmt, GUID_WICPixelFormat24bppBGR) && SUCCEEDED(fr->WriteSource(cv, nullptr)) &&
             SUCCEEDED(fr->Commit()) && SUCCEEDED(enc->Commit());
    }
    if (ok) {
        STATSTG stat{};
        LARGE_INTEGER zero{};
        ok = SUCCEEDED(st->Stat(&stat, STATFLAG_NONAME)) && SUCCEEDED(st->Seek(zero, STREAM_SEEK_SET, nullptr));
        if (ok && stat.cbSize.QuadPart > 0 && stat.cbSize.QuadPart <= kMaxEncodedBytes &&
            stat.cbSize.QuadPart <= ULONG_MAX && stat.cbSize.QuadPart <= jpeg.max_size()) {
            try {
                jpeg.resize((size_t)stat.cbSize.QuadPart);
            } catch (const std::bad_alloc&) {
                ok = false;
            } catch (const std::length_error&) {
                ok = false;
            }
        } else {
            ok = false;
        }
        if (ok) {
            ULONG got = 0;
            ok = SUCCEEDED(st->Read(jpeg.data(), (ULONG)jpeg.size(), &got)) && got == jpeg.size();
        }
    }
    Rel(bag); Rel(fr); Rel(enc); Rel(st); Rel(cv); Rel(sc);
    if (!ok) jpeg.clear();
    return ok;
}

// ---- the worker: one job at a time, newest wins ---------------------------------------------
struct Job { bool capture = false; PhotoImg frame; std::wstring path; uint64_t generation = 0; };
std::mutex g_jobMx;
std::condition_variable g_jobCv;
std::deque<Job> g_jobs;
std::atomic<int> g_state{0};          // 0 none, 1 working, 2 ready, 3 failed
std::string g_resultJpeg;
PhotoImg g_resultPreview;
bool g_workerStarted;
uint64_t g_photoGeneration;

unsigned __stdcall Worker(void*) {
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
        try {
            IWICImagingFactory* f = Wic();
            uint64_t frameBytes = 0;
            if (f && j.capture && ValidRgba(j.frame, frameBytes)) {
                if (g_cfg.savePhotos && !(Steam_Available() && Steam_SaveScreenshot(j.frame))) Photo_SaveOwn(j.frame);
                ComOwner<IWICBitmap> bmp;
                if (SUCCEEDED(f->CreateBitmapFromMemory(j.frame.w, j.frame.h, GUID_WICPixelFormat32bppRGBA,
                                                        (UINT)((uint64_t)j.frame.w * 4), (UINT)frameBytes,
                                                        j.frame.rgba.data(), bmp.Put())))
                    ok = EncodeSource(bmp.Get(), kSendSide, kQuality, jpeg) &&
                         ToRgba(bmp.Get(), kPreviewSide, preview);
            } else if (f && !j.path.empty()) {
                std::string file = Photo_ReadFile(j.path);
                IStream* st = file.empty() || file.size() > UINT_MAX ? nullptr :
                              SHCreateMemStream((const BYTE*)file.data(), (UINT)file.size());
                IWICBitmapDecoder* dec = nullptr; IWICBitmapFrameDecode* fr = nullptr;
                if (st && SUCCEEDED(f->CreateDecoderFromStream(st, nullptr, WICDecodeMetadataCacheOnDemand, &dec)) &&
                    SUCCEEDED(dec->GetFrame(0, &fr)))
                    ok = EncodeSource(fr, kSendSide, kQuality, jpeg) && ToRgba(fr, kPreviewSide, preview);
                Rel(fr); Rel(dec); Rel(st);
            }
        } catch (const std::bad_alloc&) {
            Log("photo: not enough memory to process the picture");
            ok = false;
        } catch (const std::length_error&) {
            Log("photo: picture data exceeded an implementation limit");
            ok = false;
        }
        size_t resultBytes = 0;
        int resultState = 3;
        {
            std::lock_guard<std::mutex> lk(g_jobMx);
            if (j.generation != g_photoGeneration || !g_jobs.empty()) continue;
            g_resultJpeg.swap(jpeg); g_resultPreview = std::move(preview);
            g_state = ok && !g_resultJpeg.empty() && g_resultJpeg.size() <= sms::kMaxPhoto ? 2 : 3;
            resultBytes = g_resultJpeg.size();
            resultState = g_state;
        }
        Log("photo: %s (%zu KB)", resultState == 2 ? "ready to send" : "couldn't be made", resultBytes / 1024);
    }
}
void Queue(Job&& j) {
    std::lock_guard<std::mutex> lk(g_jobMx);
    if (!g_workerStarted) {
        uintptr_t thread = _beginthreadex(nullptr, 0, Worker, nullptr, 0, nullptr);
        if (!thread) {
            ++g_photoGeneration;
            g_jobs.clear(); g_resultJpeg.clear(); g_resultPreview = PhotoImg{};
            g_state = 3;
            Log("photo: couldn't start the image worker (error %d)", errno);
            return;
        }
        if (!CloseHandle((HANDLE)thread)) Log("photo: couldn't close the image worker handle (error %lu)", GetLastError());
        g_workerStarted = true;
    }
    j.generation = g_photoGeneration;
    try {
        g_jobs.clear();
        g_jobs.push_back(std::move(j));
    } catch (const std::bad_alloc&) {
        ++g_photoGeneration;
        g_jobs.clear(); g_resultJpeg.clear(); g_resultPreview = PhotoImg{};
        g_state = 3;
        Log("photo: couldn't queue the image (not enough memory)");
        return;
    } catch (const std::length_error&) {
        ++g_photoGeneration;
        g_jobs.clear(); g_resultJpeg.clear(); g_resultPreview = PhotoImg{};
        g_state = 3;
        Log("photo: couldn't queue the image (queue limit reached)");
        return;
    }
    g_state = 1;
    g_jobCv.notify_one();
}
}   // namespace

std::string Photo_ReadFile(const std::wstring& path) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return {};
    LARGE_INTEGER sz{};
    std::string s;
    if (GetFileSizeEx(h, &sz) && sz.QuadPart > 0 && sz.QuadPart < 64ll * 1024 * 1024 &&
        (uint64_t)sz.QuadPart <= UINT_MAX) {
        try {
            s.resize((size_t)sz.QuadPart);
        } catch (const std::bad_alloc&) {
            Log("photo: not enough memory to read %zu MB", (size_t)sz.QuadPart / (1024 * 1024));
            CloseHandle(h);
            return {};
        } catch (const std::length_error&) {
            Log("photo: image file is too large to read");
            CloseHandle(h);
            return {};
        }
        DWORD got = 0;
        if (!ReadFile(h, s.data(), (DWORD)s.size(), &got, nullptr) || got != s.size()) s.clear();
    }
    CloseHandle(h);
    return s;
}

bool Photo_Decode(const std::string& bytes, int maxSide, PhotoImg& out) {
    out = PhotoImg{};
    IWICImagingFactory* f = Wic();
    if (!f || bytes.empty() || bytes.size() >= 64ull * 1024 * 1024 ||
        bytes.size() > UINT_MAX || maxSide <= 0 || maxSide > kMaxImageSide) return false;
    IStream* st = SHCreateMemStream((const BYTE*)bytes.data(), (UINT)bytes.size());
    IWICBitmapDecoder* dec = nullptr; IWICBitmapFrameDecode* fr = nullptr;
    bool ok = st && SUCCEEDED(f->CreateDecoderFromStream(st, nullptr, WICDecodeMetadataCacheOnDemand, &dec)) &&
              SUCCEEDED(dec->GetFrame(0, &fr)) && ToRgba(fr, maxSide, out);
    Rel(fr); Rel(dec); Rel(st);
    return ok;
}

bool Photo_EncodeJpeg(const PhotoImg& img, int maxSide, std::string& jpeg) {
    jpeg.clear();
    uint64_t bytes = 0;
    if (!ValidRgba(img, bytes) || maxSide <= 0 || maxSide > kMaxImageSide) return false;
    IWICImagingFactory* f = Wic();
    ComOwner<IWICBitmap> bmp;
    if (!f || FAILED(f->CreateBitmapFromMemory(img.w, img.h, GUID_WICPixelFormat32bppRGBA,
                                               (UINT)((uint64_t)img.w * 4), (UINT)bytes,
                                               (BYTE*)img.rgba.data(), bmp.Put()))) return false;
    return EncodeSource(bmp.Get(), maxSide, kQuality, jpeg);
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
    if (data.size() > MAXDWORD) return false;
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD w = 0;
    bool ok = WriteFile(h, data.data(), (DWORD)data.size(), &w, nullptr) && w == data.size();
    if (!CloseHandle(h)) ok = false;
    if (!ok) DeleteFileW(path.c_str());
    return ok;
}
bool Photo_SaveOwn(const PhotoImg& img) {
    uint64_t bytes = 0;
    if (!ValidRgba(img, bytes)) return false;
    IWICImagingFactory* f = Wic();
    ComOwner<IWICBitmap> bmp;
    if (!f || FAILED(f->CreateBitmapFromMemory(img.w, img.h, GUID_WICPixelFormat32bppRGBA,
                                               (UINT)((uint64_t)img.w * 4), (UINT)bytes,
                                               (BYTE*)img.rgba.data(), bmp.Put()))) return false;
    std::string full, thumb;
    bool fullOk = EncodeSource(bmp.Get(), 1 << 16, 0.92f, full);
    bool thumbOk = fullOk && EncodeSource(bmp.Get(), 240, kQuality, thumb);
    bmp.Reset();
    if (!fullOk) {
        Log("photo: couldn't encode the full-size copy");
        return false;
    }
    std::wstring dir = Photo_OwnFolder();
    CreateDirectoryW(dir.c_str(), nullptr);
    CreateDirectoryW((dir + L"\\thumbnails").c_str(), nullptr);
    SYSTEMTIME t; GetLocalTime(&t);
    wchar_t base[64];
    swprintf_s(base, L"SnowRunner_%04u%02u%02u_%02u%02u%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    std::wstring name;
    bool nameAvailable = false;
    for (int n = 1; n < 100; n++) {   // two in the same second get _2, _3...
        name = std::wstring(base) + (n > 1 ? L"_" + std::to_wstring(n) : L"") + L".jpg";
        if (GetFileAttributesW((dir + L"\\" + name).c_str()) == INVALID_FILE_ATTRIBUTES) {
            nameAvailable = true;
            break;
        }
    }
    bool ok = nameAvailable && WriteWholeFile(dir + L"\\" + name, full);
    if (ok && thumbOk && !WriteWholeFile(dir + L"\\thumbnails\\" + name, thumb))
        Log("photo: saved the photo but couldn't save its thumbnail");
    else if (ok && !thumbOk)
        Log("photo: saved the photo but couldn't make its thumbnail");
    char u[MAX_PATH * 3] = {};
    WideCharToMultiByte(CP_UTF8, 0, (dir + L"\\" + name).c_str(), -1, u, sizeof u, nullptr, nullptr);
    if (ok) Log("photo: saved %s (%dx%d, no Steam)", u, img.w, img.h);
    else Log("photo: couldn't save the photo in %s", u);
    return ok;
}

void Photo_FromCapture(PhotoImg&& frame) { Job j; j.capture = true; j.frame = std::move(frame); Queue(std::move(j)); }
void Photo_FromFile(const std::wstring& path) { Job j; j.path = path; Queue(std::move(j)); }
void Photo_Failed() {
    std::lock_guard<std::mutex> lk(g_jobMx);
    ++g_photoGeneration;
    g_jobs.clear();
    g_resultJpeg.clear(); g_resultPreview = PhotoImg{};
    g_state = 3;
}
int Photo_State() { return g_state; }
bool Photo_Take(std::string& jpeg, PhotoImg& preview) {
    std::lock_guard<std::mutex> lk(g_jobMx);
    if (g_state != 2) return false;
    jpeg.swap(g_resultJpeg); preview = std::move(g_resultPreview);
    g_resultJpeg.clear(); g_resultPreview = PhotoImg{};
    g_state = 0;
    return true;
}
void Photo_Cancel() {
    std::lock_guard<std::mutex> lk(g_jobMx);
    ++g_photoGeneration;
    g_jobs.clear();
    g_resultJpeg.clear(); g_resultPreview = PhotoImg{};
    g_state = 0;
}

// ---- decoding pictures for the phone (thumbnails, photos in a chat), on their own thread ------
namespace {
struct Dec { std::string key; std::wstring path; int maxSide; };
struct Done { std::string key; bool ok; PhotoImg img; };
constexpr size_t kMaxDecodePending = 128;
constexpr size_t kMaxDecodeDone = 64;
std::mutex g_decMx;
std::condition_variable g_decCv;
std::deque<Dec> g_decQ;
std::deque<Done> g_decDone;
std::set<std::string> g_decPending;
bool g_decStarted;
unsigned __stdcall DecWorker(void*) {
    for (;;) {
        Dec d;
        {
            std::unique_lock<std::mutex> lk(g_decMx);
            g_decCv.wait(lk, [] { return !g_decQ.empty(); });
            d = std::move(g_decQ.front()); g_decQ.pop_front();
        }
        Done r; r.key = std::move(d.key);
        try {
            std::string bytes = Photo_ReadFile(d.path);
            r.ok = Photo_Decode(bytes, d.maxSide, r.img);
        } catch (const std::bad_alloc&) {
            Log("photo: not enough memory to decode a picture");
            r.ok = false;
        } catch (const std::length_error&) {
            Log("photo: picture data exceeded an implementation limit");
            r.ok = false;
        }
        std::lock_guard<std::mutex> lk(g_decMx);
        g_decPending.erase(r.key);
        if (g_decDone.size() >= kMaxDecodeDone) {
            g_decDone.pop_front();
            static bool overflowLogged = false;
            if (!overflowLogged) {
                overflowLogged = true;
                Log("photo: discarded old decoded previews to bound memory use");
            }
        }
        try {
            g_decDone.push_back(std::move(r));
        } catch (const std::bad_alloc&) {
            Log("photo: couldn't queue a decoded preview");
        } catch (const std::length_error&) {
            Log("photo: decoded preview queue limit reached");
        }
    }
}
}   // namespace
bool Photo_DecodeAsync(const std::string& key, const std::wstring& path, int maxSide) {
    if (key.empty() || path.empty() || maxSide <= 0 || maxSide > kMaxImageSide) return false;
    std::lock_guard<std::mutex> lk(g_decMx);
    if (g_decPending.count(key)) return true;
    if (g_decPending.size() >= kMaxDecodePending) {
        Log("photo: decode queue is full");
        return false;
    }
    if (!g_decStarted) {
        uintptr_t thread = _beginthreadex(nullptr, 0, DecWorker, nullptr, 0, nullptr);
        if (!thread) {
            Log("photo: couldn't start the decode worker (error %d)", errno);
            return false;
        }
        if (!CloseHandle((HANDLE)thread)) Log("photo: couldn't close the decode worker handle (error %lu)", GetLastError());
        g_decStarted = true;
    }
    try {
        g_decPending.insert(key);
        g_decQ.push_back({ key, path, maxSide });
    } catch (const std::bad_alloc&) {
        g_decPending.erase(key);
        Log("photo: couldn't queue a thumbnail (not enough memory)");
        return false;
    } catch (const std::length_error&) {
        g_decPending.erase(key);
        Log("photo: thumbnail queue limit reached");
        return false;
    }
    g_decCv.notify_one();
    return true;
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
    if (max <= 0) return {};
    std::wstring dir;
#ifdef GITMO_TEST
    {
        std::wstring t = ModuleDir() + L"\\testshots";
        if (!gitmo_shared::IsEnvironmentVariableSet("GITMO_TEST_OWNPHOTOS") &&
            GetFileAttributesW(t.c_str()) != INVALID_FILE_ATTRIBUTES) dir = t;
    }
#endif
    // without Steam (Epic and other stores): the photos the phone kept itself
    if (dir.empty() && !(steamId && Steam_Available())) dir = Photo_OwnFolder();
    if (dir.empty()) {
        if (!steamId) return {};
        DWORD cb = 0;
        LSTATUS rc = RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath",
                                  RRF_RT_REG_SZ, nullptr, nullptr, &cb);
        if (rc != ERROR_SUCCESS || cb < sizeof(wchar_t) || cb > 32768u * sizeof(wchar_t)) return {};
        std::vector<wchar_t> steamPath((cb + sizeof(wchar_t) - 1) / sizeof(wchar_t) + 1, 0);
        rc = RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath",
                          RRF_RT_REG_SZ, nullptr, steamPath.data(), &cb);
        if (rc != ERROR_SUCCESS) return {};
        std::wstring root(steamPath.data());
        if (root.empty()) return {};
        for (auto& ch : root) if (ch == L'/') ch = L'\\';
        while (!root.empty() && root.back() == L'\\') root.pop_back();
        dir = root + L"\\userdata\\" + std::to_wstring((uint32_t)(steamId & 0xFFFFFFFFull)) +
              L"\\760\\remote\\1465360\\screenshots";
    }
    struct F { std::wstring p; FILETIME t; };
    std::vector<F> v;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*.jpg").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            auto pos = std::lower_bound(v.begin(), v.end(), fd.ftLastWriteTime,
                [](const F& a, const FILETIME& t) { return CompareFileTime(&a.t, &t) > 0; });
            if (v.size() >= (size_t)max && pos == v.end()) continue;
            v.insert(pos, { dir + L"\\" + fd.cFileName, fd.ftLastWriteTime });
            if (v.size() > (size_t)max) v.pop_back();
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    std::vector<std::wstring> out;
    out.reserve(v.size());
    for (auto& f : v) out.push_back(std::move(f.p));
    return out;
}
std::wstring Photo_SteamThumb(const std::wstring& shot) {
    size_t s = shot.find_last_of(L'\\');
    if (s == std::wstring::npos) return shot;
    std::wstring t = shot.substr(0, s) + L"\\thumbnails" + shot.substr(s);
    return GetFileAttributesW(t.c_str()) != INVALID_FILE_ATTRIBUTES ? t : shot;
}
