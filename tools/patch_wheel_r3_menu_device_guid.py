from pathlib import Path


def patch(path, lowerfn, keyfn, prefix):
    p=Path(path); s=p.read_text()
    def r(a,b,n):
        nonlocal s
        c=s.count(a)
        if c != 1: raise SystemExit(f'{prefix} {n}: {c}')
        s=s.replace(a,b,1); print('patched',prefix,n)

    r('#include <cstdint>\n#include <string>\n','#include <cstdint>\n#include <cstdio>\n#include <string>\n','cstdio')
    r('''namespace Settings\n{\n    extern Setting<bool> WheelUniversalSetupEnable;\n''','''namespace Settings\n{\n    extern Setting<bool> WheelUniversalSetupEnable;\n    extern Setting<std::string> WheelFFBDeviceGuid;\n''','extern guid')
    r('''            bool found = false;\n            bool ignoreName = false;\n''','''            bool found = false;\n            bool ignoreName = false;\n            bool matchGuidOnly = false;\n''','ctx')

    old=f'''        static std::string {lowerfn}(const char* text)\n        {{\n            std::string result = text ? text : "";\n            std::transform(result.begin(), result.end(), result.begin(),\n                [](unsigned char c) {{ return static_cast<char>(std::tolower(c)); }});\n            return result;\n        }}\n'''
    new=old+f'''\n        static std::string {keyfn}(const GUID& guid)\n        {{\n            char b[64]{{}};\n            std::snprintf(b, sizeof(b),\n                "%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X",\n                (unsigned)guid.Data1, (unsigned)guid.Data2, (unsigned)guid.Data3,\n                (unsigned)guid.Data4[0], (unsigned)guid.Data4[1],\n                (unsigned)guid.Data4[2], (unsigned)guid.Data4[3],\n                (unsigned)guid.Data4[4], (unsigned)guid.Data4[5],\n                (unsigned)guid.Data4[6], (unsigned)guid.Data4[7]);\n            return {lowerfn}(b);\n        }}\n'''
    r(old,new,'guid helper')

    old=f'''            const std::string wanted = {lowerfn}(Settings::WheelMenuR3DeviceName.get().c_str());\n            const std::string instanceName = {lowerfn}(instance->tszInstanceName);\n            const std::string productName = {lowerfn}(instance->tszProductName);\n\n            if (!ctx->ignoreName && !wanted.empty() &&\n'''
    new=f'''            const std::string wanted = {lowerfn}(Settings::WheelMenuR3DeviceName.get().c_str());\n            const std::string wantedGuid = {lowerfn}(Settings::WheelFFBDeviceGuid.get().c_str());\n            const std::string instanceName = {lowerfn}(instance->tszInstanceName);\n            const std::string productName = {lowerfn}(instance->tszProductName);\n\n            if (ctx->matchGuidOnly)\n            {{\n                if (wantedGuid.empty() || {keyfn}(instance->guidInstance) != wantedGuid)\n                    return DIENUM_CONTINUE;\n            }}\n            else if (!ctx->ignoreName && !wanted.empty() &&\n'''
    r(old,new,'callback')

    old=(
        '            EnumContext ctx{};\n'
        '            hr = directInput->EnumDevices(\n'
        '                DI8DEVCLASS_GAMECTRL,\n'
        '                enumDevicesCallback,\n'
        '                &ctx,\n'
        '                DIEDFL_ATTACHEDONLY | DIEDFL_FORCEFEEDBACK);\n'
        '            if (SUCCEEDED(hr) && !ctx.found &&\n'
        f'                {lowerfn}(Settings::WheelMenuR3DeviceName.get().c_str()) == "moza")\n'
        '            {\n'
        '                ctx.ignoreName = true;\n'
        '                spdlog::warn(\n'
        f'                    "{prefix}: no literal MOZA name; trying first attached FFB wheel");\n'
        '                hr = directInput->EnumDevices(\n'
        '                    DI8DEVCLASS_GAMECTRL,\n'
        '                    enumDevicesCallback,\n'
        '                    &ctx,\n'
        '                    DIEDFL_ATTACHEDONLY | DIEDFL_FORCEFEEDBACK);\n'
        '            }\n'
    )
    new=(
        '            EnumContext ctx{};\n'
        '            const std::string configuredGuid =\n'
        f'                {lowerfn}(Settings::WheelFFBDeviceGuid.get().c_str());\n'
        '            if (!configuredGuid.empty())\n'
        '            {\n'
        '                ctx.matchGuidOnly = true;\n'
        '                hr = directInput->EnumDevices(\n'
        '                    DI8DEVCLASS_GAMECTRL, enumDevicesCallback, &ctx,\n'
        '                    DIEDFL_ATTACHEDONLY | DIEDFL_FORCEFEEDBACK);\n'
        '                if (SUCCEEDED(hr) && !ctx.found)\n'
        '                {\n'
        '                    spdlog::warn(\n'
        f'                        "{prefix}: saved FFB GUID not attached; falling back to menu device name");\n'
        '                    ctx = {};\n'
        '                }\n'
        '            }\n\n'
        '            if (!ctx.found)\n'
        '            {\n'
        '                hr = directInput->EnumDevices(\n'
        '                    DI8DEVCLASS_GAMECTRL, enumDevicesCallback, &ctx,\n'
        '                    DIEDFL_ATTACHEDONLY | DIEDFL_FORCEFEEDBACK);\n'
        '            }\n'
        '            if (SUCCEEDED(hr) && !ctx.found &&\n'
        f'                {lowerfn}(Settings::WheelMenuR3DeviceName.get().c_str()) == "moza")\n'
        '            {\n'
        '                ctx.ignoreName = true;\n'
        '                spdlog::warn(\n'
        f'                    "{prefix}: no literal MOZA name; trying first attached FFB wheel");\n'
        '                hr = directInput->EnumDevices(\n'
        '                    DI8DEVCLASS_GAMECTRL, enumDevicesCallback, &ctx,\n'
        '                    DIEDFL_ATTACHEDONLY | DIEDFL_FORCEFEEDBACK);\n'
        '            }\n'
    )
    r(old,new,'guid-first selection')
    p.write_text(s)

patch('src/hooks_wheel_r3_menu_dpad.hpp','r3_lower_copy','r3DPadGuidKey','WheelMenuR3DirectDPad')
patch('src/hooks_wheel_r3_menu_ab.hpp','lowerCopy','r3ABGuidKey','WheelMenuR3DirectAB')
print('Applied exact FFB GUID preference to fixed R3 menu readers')
