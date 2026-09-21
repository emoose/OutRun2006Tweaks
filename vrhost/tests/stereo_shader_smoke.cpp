#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <d3dcompiler.h>
#include <cstring>
#include <iostream>
#include "stereo_shader.hpp"

static bool Compile(const char* entry, const char* target)
{
    ID3DBlob* code = nullptr;
    ID3DBlob* errors = nullptr;
    const HRESULT hr = D3DCompile(OutRunStereoBlitShader, std::strlen(OutRunStereoBlitShader),
        "OutRunStereoBlit", nullptr, nullptr, entry, target,
        D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
    if (FAILED(hr))
    {
        std::cerr << entry << "/" << target << " failed";
        if (errors && errors->GetBufferPointer())
            std::cerr << ": " << static_cast<const char*>(errors->GetBufferPointer());
        std::cerr << "\n";
    }
    if (errors) errors->Release();
    if (code) code->Release();
    return SUCCEEDED(hr);
}

int main()
{
    if (!Compile("VSMain", "vs_5_0")) return 1;
    if (!Compile("PSMain", "ps_5_0")) return 2;
    std::cout << "stereo shader smoke: VSMain/PSMain compiled successfully\n";
    return 0;
}
