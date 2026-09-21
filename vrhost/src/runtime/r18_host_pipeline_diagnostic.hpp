#pragma once

// R18 host-pipeline A/B diagnostic. Forced-included only by the dedicated
// capture-diagnostic target, after R17. It keeps the normal host D3D11/OpenXR
// session but replaces final submission with deterministic probes that expose
// exact XrResult/HRESULT failure stages.

#ifdef xrEndFrame
#undef xrEndFrame
#endif

#include <array>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

namespace OutRunVrR18Diagnostic
{
    inline constexpr const char* BuildId = "R18-host-pipeline-ab-20260915";
    inline constexpr ULONGLONG PhaseDurationMs = 5000;
    inline constexpr int PhaseCount = 5;

    inline std::ofstream LogFile;
    inline ULONGLONG StartMs = 0;
    inline ULONGLONG LastDupProbeMs = 0;
    inline int LastPhase = -1;
    inline std::uint64_t Frames = 0;
    inline std::array<std::uint64_t, PhaseCount> Ok{};
    inline std::array<std::uint64_t, PhaseCount> Fail{};
    inline bool FormatsLogged = false;

    inline void Log(const std::string& text)
    {
        if (!LogFile.is_open())
            LogFile.open("outrun-vr-r18-host-pipeline.log", std::ios::out | std::ios::trunc);
        SYSTEMTIME st{}; GetLocalTime(&st);
        std::ostringstream ss;
        ss << std::setfill('0') << "[" << std::setw(2) << st.wHour << ":"
           << std::setw(2) << st.wMinute << ":" << std::setw(2) << st.wSecond
           << "." << std::setw(3) << st.wMilliseconds << "] [R18] " << text;
        std::cerr << ss.str() << "\n";
        if (LogFile) { LogFile << ss.str() << "\n"; LogFile.flush(); }
    }

    inline std::string Hr(HRESULT hr)
    {
        std::ostringstream ss; ss << "0x" << std::hex << std::uppercase
            << static_cast<std::uint32_t>(hr); return ss.str();
    }

    inline const char* PhaseName(int p)
    {
        switch (p)
        {
        case 0: return "PREFERRED-CLEAR (production format preference, no shader)";
        case 1: return "UNORM-CLEAR (R16-known-good format family, no shader)";
        case 2: return "SRGB-CLEAR (production-preferred format family, no shader)";
        case 3: return "UNORM-PRODUCTION-SHADER (synthetic source through R10 RenderTo)";
        case 4: return "DUPLICATION-PROBE (UNORM clear + exact DuplicateOutput HRESULT)";
        default: return "UNKNOWN";
        }
    }

    template <typename T> inline void ReleaseCom(T*& p)
    {
        if (p) { p->Release(); p = nullptr; }
    }

    struct ProbeSwapchain
    {
        const char* name = "unset";
        XrSwapchain handle = XR_NULL_HANDLE;
        DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
        std::uint32_t width = 1600, height = 1000, arraySize = 2;
        std::vector<XrSwapchainImageD3D11KHR> images;
        std::vector<std::array<ID3D11RenderTargetView*, 2>> rtvs;
        bool attempted = false;
        bool ready = false;

        void Destroy()
        {
            for (auto& r : rtvs) { ReleaseCom(r[0]); ReleaseCom(r[1]); }
            rtvs.clear(); images.clear();
            if (handle != XR_NULL_HANDLE) { ::xrDestroySwapchain(handle); handle = XR_NULL_HANDLE; }
            ready = false;
        }
    };

    inline ProbeSwapchain Preferred{"preferred"};
    inline ProbeSwapchain Unorm{"unorm"};
    inline ProbeSwapchain Srgb{"srgb"};

    inline bool Contains(const std::vector<std::int64_t>& f, DXGI_FORMAT v)
    {
        return std::find(f.begin(), f.end(), static_cast<std::int64_t>(v)) != f.end();
    }

    inline std::vector<std::int64_t> Formats(XrSession session)
    {
        std::uint32_t count = 0;
        const XrResult a = ::xrEnumerateSwapchainFormats(session, 0, &count, nullptr);
        if (XR_FAILED(a) || !count) { Log("xrEnumerateSwapchainFormats(count) failed result=" + std::to_string(a)); return {}; }
        std::vector<std::int64_t> out(count);
        const XrResult b = ::xrEnumerateSwapchainFormats(session, count, &count, out.data());
        if (XR_FAILED(b)) { Log("xrEnumerateSwapchainFormats(list) failed result=" + std::to_string(b)); return {}; }
        if (!FormatsLogged)
        {
            FormatsLogged = true;
            std::ostringstream ss; ss << "runtime swapchain formats:";
            for (auto v : out) ss << " " << v;
            Log(ss.str());
        }
        return out;
    }

    inline DXGI_FORMAT ChooseUnorm(XrSession s)
    {
        const auto f = Formats(s);
        for (DXGI_FORMAT v : {DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM})
            if (Contains(f, v)) return v;
        return DXGI_FORMAT_UNKNOWN;
    }

    inline DXGI_FORMAT ChooseSrgb(XrSession s)
    {
        const auto f = Formats(s);
        for (DXGI_FORMAT v : {DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB})
            if (Contains(f, v)) return v;
        return DXGI_FORMAT_UNKNOWN;
    }

    inline bool EnsureSwapchain(ProbeSwapchain& s, XrSession session, DXGI_FORMAT format)
    {
        if (s.ready) return true;
        if (s.attempted) return false;
        s.attempted = true; s.format = format;
        if (!OutRunVrFinalTest::Device) { Log(std::string(s.name) + " fail: finaltest Device=null"); return false; }
        if (format == DXGI_FORMAT_UNKNOWN) { Log(std::string(s.name) + " fail: requested format unavailable"); return false; }

        XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
        ci.format = static_cast<std::int64_t>(format);
        ci.sampleCount = 1; ci.width = s.width; ci.height = s.height;
        ci.faceCount = 1; ci.arraySize = s.arraySize; ci.mipCount = 1;
        XrResult xr = ::xrCreateSwapchain(session, &ci, &s.handle);
        if (XR_FAILED(xr)) { Log(std::string(s.name) + " xrCreateSwapchain FAIL result=" + std::to_string(xr) + " format=" + std::to_string((int)format)); return false; }

        std::uint32_t count = 0;
        xr = ::xrEnumerateSwapchainImages(s.handle, 0, &count, nullptr);
        if (XR_FAILED(xr) || !count) { Log(std::string(s.name) + " enumerate-count FAIL result=" + std::to_string(xr)); s.Destroy(); return false; }
        s.images.resize(count);
        for (auto& i : s.images) i = {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR};
        xr = ::xrEnumerateSwapchainImages(s.handle, count, &count,
            reinterpret_cast<XrSwapchainImageBaseHeader*>(s.images.data()));
        if (XR_FAILED(xr)) { Log(std::string(s.name) + " enumerate-images FAIL result=" + std::to_string(xr)); s.Destroy(); return false; }

        D3D11_TEXTURE2D_DESC td{}; s.images[0].texture->GetDesc(&td);
        {
            std::ostringstream ss; ss << s.name << " image0 desc requestedFmt=" << (int)format
                << " actualFmt=" << (int)td.Format << " bind=0x" << std::hex << td.BindFlags
                << " misc=0x" << td.MiscFlags << std::dec << " images=" << count;
            Log(ss.str());
        }

        s.rtvs.resize(count);
        for (std::uint32_t i = 0; i < count; ++i)
        {
            for (std::uint32_t slice = 0; slice < 2; ++slice)
            {
                D3D11_RENDER_TARGET_VIEW_DESC rd{};
                rd.Format = format; rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
                rd.Texture2DArray.MipSlice = 0; rd.Texture2DArray.FirstArraySlice = slice; rd.Texture2DArray.ArraySize = 1;
                const HRESULT hr = OutRunVrFinalTest::Device->CreateRenderTargetView(s.images[i].texture, &rd, &s.rtvs[i][slice]);
                if (FAILED(hr) || !s.rtvs[i][slice])
                {
                    Log(std::string(s.name) + " CreateRTV FAIL image=" + std::to_string(i) + " slice=" + std::to_string(slice) + " hr=" + Hr(hr));
                    s.Destroy(); return false;
                }
            }
        }
        s.ready = true;
        Log(std::string(s.name) + " READY format=" + std::to_string((int)format));
        return true;
    }

    inline bool RenderClear(ProbeSwapchain& s, std::uint32_t& image)
    {
        if (!s.ready || !OutRunVrFinalTest::Context) return false;
        XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        XrResult xr = ::xrAcquireSwapchainImage(s.handle, &ai, &image);
        if (XR_FAILED(xr)) { Log(std::string(s.name) + " acquire FAIL result=" + std::to_string(xr)); return false; }
        XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO}; wi.timeout = XR_INFINITE_DURATION;
        xr = ::xrWaitSwapchainImage(s.handle, &wi);
        if (XR_FAILED(xr)) { Log(std::string(s.name) + " wait FAIL result=" + std::to_string(xr)); return false; }
        if (image >= s.rtvs.size()) { Log(std::string(s.name) + " image index OOB=" + std::to_string(image)); return false; }
        const float left[4]{0.95f,0.03f,0.03f,1.f};
        const float right[4]{0.03f,0.95f,0.03f,1.f};
        OutRunVrFinalTest::Context->ClearRenderTargetView(s.rtvs[image][0], left);
        OutRunVrFinalTest::Context->ClearRenderTargetView(s.rtvs[image][1], right);
        OutRunVrFinalTest::Context->Flush();
        XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        xr = ::xrReleaseSwapchainImage(s.handle, &ri);
        if (XR_FAILED(xr)) { Log(std::string(s.name) + " release FAIL result=" + std::to_string(xr)); return false; }
        return true;
    }

    inline bool RenderShader(ProbeSwapchain& s, std::uint32_t& image)
    {
        if (!s.ready || !OutRunVrFinalTest::Context) return false;
        using namespace OutRunVrSbsCaptureOverride;
        if (!CreateShaders())
        {
            Log("production CreateShaders FAIL Vs=" + std::to_string(Vs != nullptr) + " Ps=" + std::to_string(Ps != nullptr) + " Sampler=" + std::to_string(Sampler != nullptr) + " CB=" + std::to_string(ConstantBuffer != nullptr));
            return false;
        }
        if (!OutRunVrCaptureChainDiagnostic::EnsureInternalSource()) { Log("EnsureInternalSource FAIL"); return false; }

        XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        XrResult xr = ::xrAcquireSwapchainImage(s.handle, &ai, &image);
        if (XR_FAILED(xr)) { Log("shader acquire FAIL result=" + std::to_string(xr)); return false; }
        XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO}; wi.timeout = XR_INFINITE_DURATION;
        xr = ::xrWaitSwapchainImage(s.handle, &wi);
        if (XR_FAILED(xr)) { Log("shader wait FAIL result=" + std::to_string(xr)); return false; }
        if (image >= s.rtvs.size()) return false;
        const UvRect l{0.f,0.f,0.5f,1.f}, r{0.5f,0.f,0.5f,1.f};
        bool ok = OutRunVrCaptureChainDiagnostic::RenderWithSource(s.rtvs[image][0], s.width, s.height, l,
            OutRunVrCaptureChainDiagnostic::InternalSrv, DXGI_FORMAT_B8G8R8A8_UNORM, 1.f);
        ok = OutRunVrCaptureChainDiagnostic::RenderWithSource(s.rtvs[image][1], s.width, s.height, r,
            OutRunVrCaptureChainDiagnostic::InternalSrv, DXGI_FORMAT_B8G8R8A8_UNORM, 1.f) && ok;
        if (!ok) Log("production RenderTo returned false");
        OutRunVrFinalTest::Context->Flush();
        XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        xr = ::xrReleaseSwapchainImage(s.handle, &ri);
        if (XR_FAILED(xr)) { Log("shader release FAIL result=" + std::to_string(xr)); return false; }
        return ok;
    }

    inline void FillQuad(XrCompositionLayerQuad& q, XrSwapchain swap, std::uint32_t slice, XrEyeVisibility eye)
    {
        q = {XR_TYPE_COMPOSITION_LAYER_QUAD};
        q.space = OutRunVrSbsCaptureOverride::ViewSpace;
        q.eyeVisibility = eye; q.pose.orientation.w = 1.f; q.pose.position.z = -1.5f;
        q.subImage.swapchain = swap; q.subImage.imageRect.offset = {0,0};
        q.subImage.imageRect.extent = {1600,1000}; q.subImage.imageArrayIndex = slice;
        q.size = {1.8f,1.125f};
    }

    inline bool SubmitProbe(XrSession session, ProbeSwapchain& s, bool shader,
        XrFrameEndInfo& patched, std::array<XrCompositionLayerQuad,2>& quads,
        const XrCompositionLayerBaseHeader* (&layers)[2])
    {
        if (!OutRunVrSbsCaptureOverride::EnsureViewSpace(session)) { Log("EnsureViewSpace FAIL"); return false; }
        std::uint32_t image = 0;
        const bool rendered = shader ? RenderShader(s, image) : RenderClear(s, image);
        if (!rendered) return false;
        FillQuad(quads[0], s.handle, 0, XR_EYE_VISIBILITY_LEFT);
        FillQuad(quads[1], s.handle, 1, XR_EYE_VISIBILITY_RIGHT);
        layers[0] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quads[0]);
        layers[1] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quads[1]);
        patched.layerCount = 2; patched.layers = layers;
        return true;
    }

    inline void ProbeDuplicateOutput()
    {
        const ULONGLONG now = GetTickCount64();
        if (now - LastDupProbeMs < 1000) return;
        LastDupProbeMs = now;
        using namespace OutRunVrSbsCaptureOverride;
        if (!OutRunVrFinalTest::Device) { Log("dup probe Device=null"); return; }
        HWND hwnd = FindGameWindow();
        if (!hwnd) { Log("dup probe game window unavailable"); return; }
        HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
        IDXGIDevice* dxgi = nullptr;
        HRESULT hr = OutRunVrFinalTest::Device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgi));
        if (FAILED(hr) || !dxgi) { Log("dup probe QI IDXGIDevice FAIL hr=" + Hr(hr)); return; }
        IDXGIAdapter* adapter = nullptr; hr = dxgi->GetAdapter(&adapter); dxgi->Release();
        if (FAILED(hr) || !adapter) { Log("dup probe GetAdapter FAIL hr=" + Hr(hr)); return; }
        IDXGIOutput* selected = nullptr; DXGI_OUTPUT_DESC od{};
        for (UINT i=0;;++i) { IDXGIOutput* o=nullptr; if (adapter->EnumOutputs(i,&o)==DXGI_ERROR_NOT_FOUND) break; DXGI_OUTPUT_DESC d{}; o->GetDesc(&d); if (d.Monitor==mon) { selected=o; od=d; break; } o->Release(); }
        adapter->Release();
        if (!selected) { Log("dup probe matching output unavailable"); return; }
        IDXGIOutput1* o1=nullptr; IDXGIOutput5* o5=nullptr;
        selected->QueryInterface(__uuidof(IDXGIOutput1), reinterpret_cast<void**>(&o1));
        selected->QueryInterface(__uuidof(IDXGIOutput5), reinterpret_cast<void**>(&o5));
        selected->Release();
        IDXGIOutputDuplication* dup5=nullptr; IDXGIOutputDuplication* dup1=nullptr;
        HRESULT h5=E_NOINTERFACE, h1=E_NOINTERFACE;
        if (o5) { const DXGI_FORMAT fmts[]{DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_B8G8R8A8_UNORM}; h5=o5->DuplicateOutput1(OutRunVrFinalTest::Device,0,2,fmts,&dup5); }
        if (o1) h1=o1->DuplicateOutput(OutRunVrFinalTest::Device,&dup1);
        std::ostringstream ss; ss << "dup probe output=" << (od.DesktopCoordinates.right-od.DesktopCoordinates.left) << "x" << (od.DesktopCoordinates.bottom-od.DesktopCoordinates.top)
            << " DuplicateOutput1=" << Hr(h5) << " ptr=" << (dup5?1:0)
            << " DuplicateOutput=" << Hr(h1) << " ptr=" << (dup1?1:0)
            << " R10existingDup=" << (Duplication?1:0);
        Log(ss.str());
        ReleaseCom(dup5); ReleaseCom(dup1); ReleaseCom(o5); ReleaseCom(o1);
    }

    inline XrResult XRAPI_CALL EndFrame(XrSession session, const XrFrameEndInfo* endInfo)
    {
        ++Frames;
        if (!StartMs)
        {
            StartMs = GetTickCount64();
            Log(std::string("build=") + BuildId);
            Log("normal host session/device retained; final layer replaced only by R18 probes");
        }
        if (!endInfo) return ::xrEndFrame(session, endInfo);
        const int phase = static_cast<int>(((GetTickCount64()-StartMs)/PhaseDurationMs)%PhaseCount);
        if (phase != LastPhase) { LastPhase = phase; Log(std::string("PHASE ") + PhaseName(phase)); }

        XrFrameEndInfo patched = *endInfo;
        std::array<XrCompositionLayerQuad,2> quads{};
        const XrCompositionLayerBaseHeader* layers[2]{};
        bool ready = false;

        if (phase == 0)
        {
            const DXGI_FORMAT fmt = OutRunVrSbsCaptureOverride::ChooseSwapchainFormat(session);
            if (!Preferred.attempted) Log("production preferred format=" + std::to_string((int)fmt));
            if (EnsureSwapchain(Preferred, session, fmt)) ready = SubmitProbe(session, Preferred, false, patched, quads, layers);
        }
        else if (phase == 1)
        {
            if (EnsureSwapchain(Unorm, session, ChooseUnorm(session))) ready = SubmitProbe(session, Unorm, false, patched, quads, layers);
        }
        else if (phase == 2)
        {
            if (EnsureSwapchain(Srgb, session, ChooseSrgb(session))) ready = SubmitProbe(session, Srgb, false, patched, quads, layers);
        }
        else if (phase == 3)
        {
            if (EnsureSwapchain(Unorm, session, ChooseUnorm(session))) ready = SubmitProbe(session, Unorm, true, patched, quads, layers);
        }
        else
        {
            ProbeDuplicateOutput();
            if (EnsureSwapchain(Unorm, session, ChooseUnorm(session))) ready = SubmitProbe(session, Unorm, false, patched, quads, layers);
        }

        if (ready) ++Ok[phase]; else ++Fail[phase];
        if ((Frames % 300u) == 0u)
        {
            std::ostringstream ss; ss << "summary frames=" << Frames << " ok=[";
            for (int i=0;i<PhaseCount;++i){if(i)ss<<",";ss<<Ok[i];}
            ss << "] fail=["; for(int i=0;i<PhaseCount;++i){if(i)ss<<",";ss<<Fail[i];}
            ss << "] Device=" << (OutRunVrFinalTest::Device?1:0) << " Context=" << (OutRunVrFinalTest::Context?1:0)
               << " R10capture[fresh=" << OutRunVrSbsCaptureOverride::CaptureFresh << ",fail=" << OutRunVrSbsCaptureOverride::CaptureFailure << "]";
            Log(ss.str());
        }
        return ::xrEndFrame(session, ready ? &patched : endInfo);
    }
}

#define xrEndFrame OutRunVrR18Diagnostic::EndFrame
