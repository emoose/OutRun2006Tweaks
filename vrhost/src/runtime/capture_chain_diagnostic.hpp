#pragma once

// R17 capture-chain diagnostic.
// This header is forced-included only by the dedicated diagnostic executable.
// It deliberately reuses the production R10 Desktop Duplication, crop, shader,
// swapchain and projection helpers, then submits controlled phases directly to
// the OpenXR loader. The normal outrun-vr-host target never includes this file.

#ifdef xrEndFrame
#undef xrEndFrame
#endif

#include <fstream>
#include <iomanip>
#include <sstream>

namespace OutRunVrCaptureChainDiagnostic
{
    inline constexpr const char* BuildId = "R17-capture-chain-diagnostic-20260915";
    inline constexpr ULONGLONG PhaseDurationMs = 5000;
    inline constexpr int PhaseCount = 5;

    inline ULONGLONG StartMs = 0;
    inline ULONGLONG LastProbeMs = 0;
    inline int LastPhase = -1;
    inline std::uint64_t Frames = 0;
    inline std::array<std::uint64_t, PhaseCount> PhaseOk{};
    inline std::array<std::uint64_t, PhaseCount> PhaseFail{};
    inline std::ofstream LogFile;

    inline OutRunVrSbsCaptureOverride::Swapchain DiagArray{};
    inline ID3D11Texture2D* InternalTexture = nullptr;
    inline ID3D11ShaderResourceView* InternalSrv = nullptr;
    inline ID3D11Texture2D* ProbeStaging = nullptr;
    inline std::uint32_t ProbeWidth = 0;
    inline std::uint32_t ProbeHeight = 0;
    inline DXGI_FORMAT ProbeFormat = DXGI_FORMAT_UNKNOWN;

    inline void EnsureLog()
    {
        if (!LogFile.is_open())
            LogFile.open("outrun-vr-capture-diagnostic.log",
                std::ios::out | std::ios::trunc);
    }

    inline void Log(const std::string& text)
    {
        EnsureLog();
        SYSTEMTIME st{};
        GetLocalTime(&st);
        std::ostringstream ss;
        ss << std::setfill('0')
           << "[" << std::setw(2) << st.wHour << ":"
           << std::setw(2) << st.wMinute << ":"
           << std::setw(2) << st.wSecond << "."
           << std::setw(3) << st.wMilliseconds << "] "
           << "[R17] " << text;
        std::cerr << ss.str() << "\n";
        if (LogFile)
        {
            LogFile << ss.str() << "\n";
            LogFile.flush();
        }
    }

    inline const char* PhaseName(int phase)
    {
        switch (phase)
        {
        case 0: return "1/5 INTERNAL-BLIT: synthetic LEFT RED / RIGHT GREEN through production shader";
        case 1: return "2/5 DESKTOP-WHOLE: duplicated monitor shown identically to both eyes";
        case 2: return "3/5 GAME-CROP: game client crop shown identically to both eyes";
        case 3: return "4/5 GAME-SBS-EYE: game crop split left/right into eye-specific quads";
        case 4: return "5/5 EXACT-PROJECTION: production R10 capture + SBS split + projection override";
        default: return "unknown";
        }
    }

    inline float HalfToFloat(std::uint16_t h)
    {
        const std::uint32_t sign = (static_cast<std::uint32_t>(h & 0x8000u)) << 16;
        std::uint32_t exp = (h >> 10) & 0x1fu;
        std::uint32_t mant = h & 0x03ffu;
        std::uint32_t bits = 0;
        if (exp == 0)
        {
            if (mant == 0)
            {
                bits = sign;
            }
            else
            {
                int e = -14;
                while ((mant & 0x0400u) == 0)
                {
                    mant <<= 1;
                    --e;
                }
                mant &= 0x03ffu;
                bits = sign |
                    (static_cast<std::uint32_t>(e + 127) << 23) |
                    (mant << 13);
            }
        }
        else if (exp == 31)
        {
            bits = sign | 0x7f800000u | (mant << 13);
        }
        else
        {
            bits = sign | ((exp + (127 - 15)) << 23) | (mant << 13);
        }

        float value = 0.f;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    }

    struct Pixel
    {
        float r = 0.f;
        float g = 0.f;
        float b = 0.f;
        float a = 1.f;
    };

    inline Pixel ReadPixel(const D3D11_MAPPED_SUBRESOURCE& mapped,
        std::uint32_t x, std::uint32_t y, DXGI_FORMAT format)
    {
        Pixel p{};
        const auto* row = static_cast<const std::uint8_t*>(mapped.pData) +
            static_cast<std::size_t>(y) * mapped.RowPitch;
        if (format == DXGI_FORMAT_B8G8R8A8_UNORM)
        {
            const auto* s = row + static_cast<std::size_t>(x) * 4u;
            p.b = static_cast<float>(s[0]) / 255.f;
            p.g = static_cast<float>(s[1]) / 255.f;
            p.r = static_cast<float>(s[2]) / 255.f;
            p.a = static_cast<float>(s[3]) / 255.f;
        }
        else if (format == DXGI_FORMAT_R16G16B16A16_FLOAT)
        {
            const auto* s = reinterpret_cast<const std::uint16_t*>(
                row + static_cast<std::size_t>(x) * 8u);
            p.r = HalfToFloat(s[0]);
            p.g = HalfToFloat(s[1]);
            p.b = HalfToFloat(s[2]);
            p.a = HalfToFloat(s[3]);
        }
        return p;
    }

    inline bool EnsureProbeStaging()
    {
        using namespace OutRunVrSbsCaptureOverride;
        if (!Source || !OutRunVrFinalTest::Device)
            return false;

        D3D11_TEXTURE2D_DESC src{};
        Source->GetDesc(&src);
        if (ProbeStaging && ProbeWidth == src.Width &&
            ProbeHeight == src.Height && ProbeFormat == src.Format)
            return true;

        ReleaseCom(ProbeStaging);
        D3D11_TEXTURE2D_DESC d = src;
        d.MipLevels = 1;
        d.ArraySize = 1;
        d.SampleDesc.Count = 1;
        d.SampleDesc.Quality = 0;
        d.Usage = D3D11_USAGE_STAGING;
        d.BindFlags = 0;
        d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        d.MiscFlags = 0;
        if (FAILED(OutRunVrFinalTest::Device->CreateTexture2D(
                &d, nullptr, &ProbeStaging)) || !ProbeStaging)
            return false;

        ProbeWidth = d.Width;
        ProbeHeight = d.Height;
        ProbeFormat = d.Format;
        return true;
    }

    inline std::string PixelText(const Pixel& p)
    {
        std::ostringstream ss;
        ss << std::fixed << std::setprecision(3)
           << "(" << p.r << "," << p.g << "," << p.b << ")";
        return ss.str();
    }

    inline void ProbeCapture()
    {
        using namespace OutRunVrSbsCaptureOverride;
        const ULONGLONG now = GetTickCount64();
        if (now - LastProbeMs < 1000 || !Source || !HaveSource ||
            !OutRunVrFinalTest::Context || !EnsureProbeStaging())
            return;
        LastProbeMs = now;

        OutRunVrFinalTest::Context->CopyResource(ProbeStaging, Source);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        const HRESULT hr = OutRunVrFinalTest::Context->Map(
            ProbeStaging, 0, D3D11_MAP_READ, 0, &mapped);
        if (FAILED(hr))
        {
            std::ostringstream ss;
            ss << "capture probe Map failed HRESULT=0x"
               << std::hex << static_cast<std::uint32_t>(hr);
            Log(ss.str());
            return;
        }

        UvRect game{};
        const bool haveGameUv = GetGameUv(game);
        const std::uint32_t desktopX = SourceWidth / 2u;
        const std::uint32_t desktopY = SourceHeight / 2u;
        Pixel desktop = ReadPixel(mapped,
            std::min(desktopX, SourceWidth - 1u),
            std::min(desktopY, SourceHeight - 1u), SourceFormat);

        Pixel gameCenter{};
        if (haveGameUv)
        {
            const float gx = game.x + game.w * 0.5f;
            const float gy = game.y + game.h * 0.5f;
            const auto px = std::min<std::uint32_t>(
                static_cast<std::uint32_t>(gx * SourceWidth), SourceWidth - 1u);
            const auto py = std::min<std::uint32_t>(
                static_cast<std::uint32_t>(gy * SourceHeight), SourceHeight - 1u);
            gameCenter = ReadPixel(mapped, px, py, SourceFormat);
        }

        int nonBlack = 0;
        constexpr int gridX = 9;
        constexpr int gridY = 5;
        const UvRect sample = haveGameUv ? game : UvRect{ 0.f, 0.f, 1.f, 1.f };
        for (int y = 0; y < gridY; ++y)
        {
            for (int x = 0; x < gridX; ++x)
            {
                const float u = sample.x + sample.w *
                    ((static_cast<float>(x) + 0.5f) / gridX);
                const float v = sample.y + sample.h *
                    ((static_cast<float>(y) + 0.5f) / gridY);
                const auto px = std::min<std::uint32_t>(
                    static_cast<std::uint32_t>(u * SourceWidth), SourceWidth - 1u);
                const auto py = std::min<std::uint32_t>(
                    static_cast<std::uint32_t>(v * SourceHeight), SourceHeight - 1u);
                const Pixel p = ReadPixel(mapped, px, py, SourceFormat);
                if (std::max({ std::fabs(p.r), std::fabs(p.g), std::fabs(p.b) }) > 0.005f)
                    ++nonBlack;
            }
        }
        OutRunVrFinalTest::Context->Unmap(ProbeStaging, 0);

        std::ostringstream ss;
        ss << "capture probe source=" << SourceWidth << "x" << SourceHeight
           << " fmt=" << static_cast<int>(SourceFormat)
           << " desktopCenter=" << PixelText(desktop);
        if (haveGameUv)
        {
            ss << " gameCenter=" << PixelText(gameCenter)
               << " gameUv=" << std::fixed << std::setprecision(4)
               << game.x << "," << game.y << "," << game.w << "," << game.h;
        }
        else
        {
            ss << " gameUv=UNAVAILABLE";
        }
        ss << " nonBlackGrid=" << nonBlack << "/" << (gridX * gridY)
           << " allBlack=" << (nonBlack == 0 ? "YES" : "NO");
        Log(ss.str());
    }

    inline bool EnsureInternalSource()
    {
        if (InternalTexture && InternalSrv)
            return true;
        if (!OutRunVrFinalTest::Device)
            return false;

        constexpr std::uint32_t width = 512;
        constexpr std::uint32_t height = 256;
        std::vector<std::uint32_t> pixels(width * height);
        for (std::uint32_t y = 0; y < height; ++y)
        {
            for (std::uint32_t x = 0; x < width; ++x)
            {
                // BGRA8 in little endian: red=0xFFFF0000, green=0xFF00FF00.
                pixels[static_cast<std::size_t>(y) * width + x] =
                    x < width / 2u ? 0xFFFF0000u : 0xFF00FF00u;
            }
        }

        D3D11_TEXTURE2D_DESC d{};
        d.Width = width;
        d.Height = height;
        d.MipLevels = 1;
        d.ArraySize = 1;
        d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_IMMUTABLE;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE;

        D3D11_SUBRESOURCE_DATA data{};
        data.pSysMem = pixels.data();
        data.SysMemPitch = width * 4u;
        if (FAILED(OutRunVrFinalTest::Device->CreateTexture2D(
                &d, &data, &InternalTexture)) || !InternalTexture)
            return false;
        if (FAILED(OutRunVrFinalTest::Device->CreateShaderResourceView(
                InternalTexture, nullptr, &InternalSrv)) || !InternalSrv)
        {
            OutRunVrSbsCaptureOverride::ReleaseCom(InternalTexture);
            return false;
        }
        return true;
    }

    inline bool RenderWithSource(ID3D11RenderTargetView* rtv,
        std::uint32_t width, std::uint32_t height,
        const OutRunVrSbsCaptureOverride::UvRect& uv,
        ID3D11ShaderResourceView* srv, DXGI_FORMAT format, float whiteScale)
    {
        using namespace OutRunVrSbsCaptureOverride;
        ID3D11ShaderResourceView* oldSrv = SourceSrv;
        const DXGI_FORMAT oldFormat = SourceFormat;
        const float oldWhite = SdrWhiteScale;
        SourceSrv = srv;
        SourceFormat = format;
        SdrWhiteScale = whiteScale;
        const bool ok = RenderTo(rtv, width, height, uv);
        SourceSrv = oldSrv;
        SourceFormat = oldFormat;
        SdrWhiteScale = oldWhite;
        return ok;
    }

    inline bool RenderDiagnosticArray(XrSession session,
        const OutRunVrSbsCaptureOverride::UvRect& leftUv,
        const OutRunVrSbsCaptureOverride::UvRect& rightUv,
        bool useInternal)
    {
        using namespace OutRunVrSbsCaptureOverride;
        if (!EnsureViewSpace(session) ||
            !EnsureSwapchain(DiagArray, session, 1600, 1000, 2))
            return false;

        std::uint32_t image = 0;
        if (!Acquire(DiagArray, image))
            return false;
        if (image >= DiagArray.rtvs.size())
        {
            Release(DiagArray);
            return false;
        }

        bool ok = false;
        if (useInternal)
        {
            if (EnsureInternalSource())
            {
                ok = RenderWithSource(DiagArray.rtvs[image][0],
                    DiagArray.width, DiagArray.height, leftUv,
                    InternalSrv, DXGI_FORMAT_B8G8R8A8_UNORM, 1.f);
                ok = RenderWithSource(DiagArray.rtvs[image][1],
                    DiagArray.width, DiagArray.height, rightUv,
                    InternalSrv, DXGI_FORMAT_B8G8R8A8_UNORM, 1.f) && ok;
            }
        }
        else if (SourceSrv && HaveSource)
        {
            ok = RenderTo(DiagArray.rtvs[image][0],
                DiagArray.width, DiagArray.height, leftUv);
            ok = RenderTo(DiagArray.rtvs[image][1],
                DiagArray.width, DiagArray.height, rightUv) && ok;
        }

        const bool released = Release(DiagArray);
        return ok && released;
    }

    inline void FillQuad(XrCompositionLayerQuad& quad,
        XrEyeVisibility eye, std::uint32_t slice, float aspect)
    {
        using namespace OutRunVrSbsCaptureOverride;
        quad = { XR_TYPE_COMPOSITION_LAYER_QUAD };
        quad.space = ViewSpace;
        quad.eyeVisibility = eye;
        quad.pose.orientation.w = 1.f;
        quad.pose.position.z = -1.5f;
        quad.subImage.swapchain = DiagArray.handle;
        quad.subImage.imageRect.offset = { 0, 0 };
        quad.subImage.imageRect.extent = {
            static_cast<std::int32_t>(DiagArray.width),
            static_cast<std::int32_t>(DiagArray.height)
        };
        quad.subImage.imageArrayIndex = slice;
        quad.size.width = 1.8f;
        quad.size.height = 1.8f / std::clamp(aspect, 0.5f, 3.0f);
    }

    inline float UvAspect(const OutRunVrSbsCaptureOverride::UvRect& uv)
    {
        using namespace OutRunVrSbsCaptureOverride;
        if (!SourceWidth || !SourceHeight || uv.h <= 0.f)
            return 16.f / 9.f;
        return (uv.w * static_cast<float>(SourceWidth)) /
            (uv.h * static_cast<float>(SourceHeight));
    }

    inline bool BuildQuadPhase(XrSession session, int phase,
        std::array<XrCompositionLayerQuad, 2>& quads, std::uint32_t& layerCount)
    {
        using namespace OutRunVrSbsCaptureOverride;
        layerCount = 0;

        if (phase == 0)
        {
            const UvRect left{ 0.f, 0.f, 0.5f, 1.f };
            const UvRect right{ 0.5f, 0.f, 0.5f, 1.f };
            if (!RenderDiagnosticArray(session, left, right, true))
                return false;
            FillQuad(quads[0], XR_EYE_VISIBILITY_LEFT, 0, 1.f);
            FillQuad(quads[1], XR_EYE_VISIBILITY_RIGHT, 1, 1.f);
            layerCount = 2;
            return true;
        }

        if (!CaptureDesktop(8) || !HaveSource || !SourceSrv)
            return false;
        ProbeCapture();

        UvRect whole{ 0.f, 0.f, 1.f, 1.f };
        UvRect game{};
        if ((phase == 2 || phase == 3) && !GetGameUv(game))
            return false;

        if (phase == 1)
        {
            if (!RenderDiagnosticArray(session, whole, whole, false))
                return false;
            FillQuad(quads[0], XR_EYE_VISIBILITY_BOTH, 0,
                static_cast<float>(SourceWidth) / std::max(1u, SourceHeight));
            layerCount = 1;
            return true;
        }

        if (phase == 2)
        {
            if (!RenderDiagnosticArray(session, game, game, false))
                return false;
            FillQuad(quads[0], XR_EYE_VISIBILITY_BOTH, 0, UvAspect(game));
            layerCount = 1;
            return true;
        }

        if (phase == 3)
        {
            const UvRect left{ game.x, game.y, game.w * 0.5f, game.h };
            const UvRect right{ game.x + game.w * 0.5f, game.y,
                game.w * 0.5f, game.h };
            if (!RenderDiagnosticArray(session, left, right, false))
                return false;
            FillQuad(quads[0], XR_EYE_VISIBILITY_LEFT, 0, UvAspect(left));
            FillQuad(quads[1], XR_EYE_VISIBILITY_RIGHT, 1, UvAspect(right));
            layerCount = 2;
            return true;
        }

        return false;
    }

    inline void MaybeSummary()
    {
        if ((Frames % 300u) != 0u)
            return;
        std::ostringstream ss;
        ss << "summary frames=" << Frames << " ok=[";
        for (int i = 0; i < PhaseCount; ++i)
        {
            if (i) ss << ",";
            ss << PhaseOk[i];
        }
        ss << "] fail=[";
        for (int i = 0; i < PhaseCount; ++i)
        {
            if (i) ss << ",";
            ss << PhaseFail[i];
        }
        ss << "] R10capture[fresh=" << OutRunVrSbsCaptureOverride::CaptureFresh
           << ",timeout=" << OutRunVrSbsCaptureOverride::CaptureTimeout
           << ",fail=" << OutRunVrSbsCaptureOverride::CaptureFailure
           << "] source=" << OutRunVrSbsCaptureOverride::SourceWidth
           << "x" << OutRunVrSbsCaptureOverride::SourceHeight
           << " fmt=" << static_cast<int>(OutRunVrSbsCaptureOverride::SourceFormat);
        Log(ss.str());
    }

    inline XrResult XRAPI_CALL EndFrame(
        XrSession session, const XrFrameEndInfo* endInfo)
    {
        ++Frames;
        if (!StartMs)
        {
            StartMs = GetTickCount64();
            Log(std::string("build=") + BuildId);
            Log("uses production R10 DuplicateOutput/GetGameUv/RenderTo/Projection helpers");
            Log("phase duration=5s; phases repeat. Keep the game running and enter gameplay.");
        }

        const int phase = static_cast<int>(
            ((GetTickCount64() - StartMs) / PhaseDurationMs) % PhaseCount);
        if (phase != LastPhase)
        {
            LastPhase = phase;
            Log(std::string("PHASE ") + PhaseName(phase));
        }

        if (!endInfo)
            return ::xrEndFrame(session, endInfo);

        XrFrameEndInfo patched = *endInfo;
        const XrCompositionLayerBaseHeader* layerPtrs[2]{};
        std::array<XrCompositionLayerQuad, 2> quads{};
        std::uint32_t layerCount = 0;
        bool ready = false;

        XrCompositionLayerProjection projection{};
        std::array<XrCompositionLayerProjectionView, 2> projectionViews{};

        if (phase < 4)
        {
            ready = BuildQuadPhase(session, phase, quads, layerCount);
            if (ready)
            {
                for (std::uint32_t i = 0; i < layerCount; ++i)
                    layerPtrs[i] =
                        reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quads[i]);
                patched.layerCount = layerCount;
                patched.layers = layerPtrs;
            }
        }
        else
        {
            // This is the exact production R10 path: capture desktop, crop the
            // game, split SBS, render through the production shader and submit
            // a two-slice projection layer using incoming or Frame.v2 poses.
            ready = OutRunVrSbsCaptureOverride::RenderProjectionOverride(
                session, endInfo, projection, projectionViews);
            ProbeCapture();
            if (ready)
            {
                layerPtrs[0] =
                    reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection);
                patched.layerCount = 1;
                patched.layers = layerPtrs;
            }
            else if ((Frames % 90u) == 0u)
            {
                std::ostringstream ss;
                ss << "exact projection not ready frameMeta="
                   << (OutRunVrSbsCaptureOverride::LastStereoFrameValid ? 1 : 0)
                   << " haveSource=" << (OutRunVrSbsCaptureOverride::HaveSource ? 1 : 0)
                   << " mainLayerCount=" << endInfo->layerCount;
                Log(ss.str());
            }
        }

        if (ready)
            ++PhaseOk[phase];
        else
            ++PhaseFail[phase];

        MaybeSummary();
        return ::xrEndFrame(session, ready ? &patched : endInfo);
    }
}

#define xrEndFrame OutRunVrCaptureChainDiagnostic::EndFrame
