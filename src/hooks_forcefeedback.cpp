#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <shellapi.h>
#include "hook_mgr.hpp"
#include "plugin.hpp"
#include "game_addrs.hpp"

#include "Xinput.h"

int VibrationUserId = 0;
int VibrationStrength = 10;
float VibrationLeftMotor = 0.f;
float VibrationRightMotor = 0.f;

void SetVibration(int userId, float leftMotor, float rightMotor)
{
    if (!Settings::VibrationMode)
        return;
    else if (Settings::VibrationMode == 2) // Swap L/R
    {
        float left = leftMotor;
        leftMotor = rightMotor;
        rightMotor = left;
    }
    else if (Settings::VibrationMode == 3) // Merge L/R by using whichever is highest
    {
        leftMotor = rightMotor = max(leftMotor, rightMotor);
    }

    XINPUT_VIBRATION vib{ 0 };
    vib.wLeftMotorSpeed = uint16_t(std::clamp(int(leftMotor * 65535.f), 0, 0xFFFF));
    vib.wRightMotorSpeed = uint16_t(std::clamp(int(rightMotor * 65535.f), 0, 0xFFFF));

    void InputManager_SetVibration(WORD, WORD);
    InputManager_SetVibration(vib.wLeftMotorSpeed, vib.wRightMotorSpeed);

	if (!Settings::UseNewInput)
		XInputSetState(userId, &vib);
}

extern "C"
{
    void __cdecl CalcVibrationValues(EVWORK_CAR* car);
    long _ftol2(double);
}

class Vibration : public Hook
{
    const static int GamePlCar_Ctrl_Addr = 0xA8330;

    inline static SafetyHookInline GamePlCar_Ctrl = {};
    static void GamePlCar_Ctrl_Hook(EVWORK_CAR* car)
    {
        CalcVibrationValues(car);
        SetVibration(0, VibrationLeftMotor, VibrationRightMotor);

        GamePlCar_Ctrl.call(car);
    }

public:
    std::string_view description() override
    {
        return "Vibration";
    }

    bool validate() override
    {
        return Settings::VibrationMode != 0 || Settings::UseNewInput;
    }

    bool apply() override
    {
        VibrationStrength = Settings::VibrationStrength;
        VibrationUserId = Settings::VibrationControllerId;

        GamePlCar_Ctrl = safetyhook::create_inline(Module::exe_ptr(GamePlCar_Ctrl_Addr), GamePlCar_Ctrl_Hook);

        return true;
    }

    static Vibration instance;
};
Vibration Vibration::instance;

float Fabsf(float f)
{
    return fabs(f);
}

int GetControllerId()
{
    return VibrationUserId;
}

const float FLOAT_0_1 = 0.1f;
const float FLOAT_0_5 = 0.5f;
const float FLOAT_0_3 = 0.3f;
const float FLOAT_2_0 = 2.0f;
const float FLOAT_0_01 = 0.01f;
const float FLOAT_0_7 = 0.7f;
const float FLOAT_0_95 = 0.95f;
const float FLOAT_0_75 = 0.75f;
const float FLOAT_5_0 = 5.0f;
const float FLOAT_0_00001 = 0.00001f;
const float FLOAT_1_5 = 1.5f;
const float FLOAT_10_0 = 10.0f;
const float FLOAT_1_0 = 1.0f;
const float FLOAT_25_0 = 25.0f;
const float FLOAT_0_2 = 0.2f;
const float FLOAT_0_0018 = 0.0018f;
const float FLOAT_0_25 = 0.25f;
const float FLOAT_0_33 = 0.33f;
const float FLOAT_MIN_0_002 = -0.002f;
const float FLOAT_MIN_0_004 = -0.004f;
const float FLOAT_0_00000011920929 = 0.00000011920929f;
const float FLOAT_MIN_0_1 = -0.1f;
const float FLOAT_3_0 = 3.0f;
const float FLOAT_0_001 = 0.001f;

float CalcVibrationValue_VibrationMult = 1.0f;
int CalcVibrationValue_FrameCnt0 = 0;
int CalcVibrationValue_FrameCnt1 = 0;

// TODO: maybe should replace this with asm version in case decompiled ver isn't perfect match...
double __cdecl sub_1149C0(unsigned int a1, int a2, DWORD* a3)
{
    int v3; // eax
    double result = 1.f; // st7
    int v5; // eax
    int v6; // eax

    if (a1 > 0x400)
    {
        if (a1 > 0x100000)
        {
            switch (a1)
            {
            case 0x200000u:
                return 0.80000001;
            case 0x400000u:
                if (a2)
                    return 0.25;
                v5 = Game::GetNowStageNum(8);
                v6 = Game::GetStageUniqueNum(v5);
                if (v6 == 0x12 || v6 == 0x30)
                    return 0.25;
                return 0.89999998;
            case 0x800000u:
                return 0.5;
            }
        }
        else
        {
            if (a1 == 0x100000)
                return 0.70999998;
            if (a1 > 0x2000)
            {
                if (a1 == 0x8000)
                    return 0.40000001;
            }
            else if (a1 == 0x2000 || a1 == 0x800 || a1 == 0x1000)
            {
                return 0.34999999;
            }
        }
        return 0.31;
    }
    if (a1 == 0x400)
        return 0.30000001;
    if (a1 > 0x10)
    {
        if (a1 == 0x80)
            return 0.85000002;
        if (a1 == 0x100)
            return 0.44999999;
        if (a1 != 0x200)
            return 0.31;
        return 0.34999999;
    }
    if (a1 == 0x10)
        return 0.89999998;
    switch (a1)
    {
    case 1u:
        result = 0.0;
        break;
    case 2u:
        if (a2)
            return 0.25;

        v3 = Game::GetNowStageNum(8);
        switch (Game::GetStageUniqueNum(v3))
        {
        case 0xB:
        case 0x29:
            result = 0.72999996;
            *a3 |= 1u;
            break;
        case 0xD:
        case 0x2B:
            result = 0.78999996;
            *a3 |= 1u;
            break;
        case 0xE:
        case 0x2C:
            result = 0.75999999;
            *a3 |= 1u;
            break;
        default:
            return 0.25;
        }
        break;
    case 4u:
        result = 0.69999999;
        break;
    case 8u:
        return 0.85000002;
    default:
        return 0.31;
    }
    return result;
}

// CalcVibrationValues taken from C2C Xbox (0x114C60)
// Fortunately Xbox ver code is almost identical to PC in most places, same memory structure sizes/engine function calls/etc
// PC version is sadly missing this function entirely, but we can mostly get away with using the original Xbox ASM code as-is
// (a decompiled version would be nicer though, and allow for more customization, but at least we know this should be very accurate to Xbox)
__declspec(naked) void CalcVibrationValues(EVWORK_CAR* a1)
{
    __asm
    {
        mov     eax, VibrationStrength
        sub     esp, 18h
        push    ebp
        push    esi
        xor ebp, ebp
        cmp     eax, 0Ah
        push    edi
        jg      short loc_114C76
        cmp     eax, ebp
        jge     short loc_114C7D

        loc_114C76 : ; CODE XREF : CalcVibrationValues + 10
        xor eax, eax
        mov     VibrationStrength, eax

        loc_114C7D : ; CODE XREF : CalcVibrationValues + 14
        mov     esi, [esp + 28h]
        cvtsi2ss xmm0, eax
        mulss   xmm0, ds : FLOAT_0_1
        movss   CalcVibrationValue_VibrationMult, xmm0
        mov     eax, [esi + 1D0h]
        xorps   xmm0, xmm0
        mov     ecx, eax
        push    ecx; float
        movss   dword ptr[esp + 14h], xmm0
        movss   dword ptr[esp + 10h], xmm0
        mov[esp + 24h], eax
        call    Fabsf
        fstp    dword ptr[esp + 20h]
        mov     eax, [esi + 8]
        add     esp, 4
        xor edi, edi
        test    ah, 1
        jz      short loc_114CDA
        mov     edx, [esi + 4]
        shr     edx, 17h
        xor edx, eax
        and edx, 100h
        xor edx, eax
        mov[esi + 8], edx

        loc_114CDA : ; CODE XREF : CalcVibrationValues + 65
        movss   xmm0, dword ptr[esi + 1C4h]
        xorps   xmm1, xmm1
        comiss  xmm0, xmm1
        jbe     loc_115015
        test    byte ptr[esi + 8], 40h
        jz      short loc_114D14
        movss   xmm0, dword ptr[esi + 178h]
        mulss   xmm0, ds:FLOAT_0_5
        movss   dword ptr[esp + 0Ch], xmm0
        mov     edi, 14h
        jmp     loc_114F6B
        ; -------------------------------------------------------------------------- -

        loc_114D14:; CODE XREF : CalcVibrationValues + 92
        movsx   eax, word ptr[esi + 0D34h]
        mov     ecx, [esi + 5Ch]
        push    ebx
        cdq
        mov     ebx, eax
        xor ebx, edx
        lea     eax, [esp + 1Ch]
        push    eax
        sub     ebx, edx
        mov     edx, [esi + 24Ch]
        push    ecx
        push    edx
        mov[esp + 28h], ebp
        sub     ebx, 7D0h
        call    sub_1149C0
        mov     ecx, [esi + 5Ch]
        mov     edx, [esi + 250h]
        fstp    dword ptr[esp + 24h]
        lea     eax, [esp + 28h]
        push    eax
        push    ecx
        push    edx
        call    sub_1149C0
        fstp    dword ptr[esp + 44h]
        add     esp, 18h
        fld     dword ptr[esp + 18h]
        fld     dword ptr[esp + 2Ch]
        fcomip  st, st(1)
        fstp    st
        jbe     short loc_114D7C
        movss   xmm0, dword ptr[esp + 2Ch]
        movss   dword ptr[esp + 18h], xmm0

        loc_114D7C : ; CODE XREF : CalcVibrationValues + 10E
        mov     ecx, [esi + 5Ch]
        mov     edx, [esi + 254h]
        lea     eax, [esp + 1Ch]
        push    eax
        push    ecx
        push    edx
        call    sub_1149C0
        fstp    dword ptr[esp + 38h]
        fld     dword ptr[esp + 24h]
        add     esp, 0Ch
        fld     dword ptr[esp + 2Ch]
        fcomip  st, st(1)
        fstp    st
        jbe     short loc_114DB2
        movss   xmm0, dword ptr[esp + 2Ch]
        movss   dword ptr[esp + 18h], xmm0

        loc_114DB2 : ; CODE XREF : CalcVibrationValues + 144
        mov     ecx, [esi + 5Ch]
        mov     edx, [esi + 258h]
        lea     eax, [esp + 1Ch]
        push    eax
        push    ecx
        push    edx
        call    sub_1149C0
        fstp    dword ptr[esp + 38h]
        fld     dword ptr[esp + 24h]
        movss   xmm1, dword ptr[esp + 38h]
        fld     dword ptr[esp + 38h]
        add     esp, 0Ch
        fcomip  st, st(1)
        fstp    st
        ja      short loc_114DE8
        movss   xmm1, dword ptr[esp + 18h]

        loc_114DE8:; CODE XREF : CalcVibrationValues + 180
        movss   xmm3, ds : FLOAT_0_1
        comiss  xmm3, dword ptr[esi + 268h]
        movaps  xmm0, xmm1
        mulss   xmm0, dword ptr[esi + 1C4h]
        movaps  xmm4, xmm0
        mulss   xmm4, xmm3
        movss   dword ptr[esp + 14h], xmm4
        jbe     short loc_114E2F
        comiss  xmm1, ds:FLOAT_0_3
        jbe     short loc_114E2F
        xorps   xmm0, xmm0
        subss   xmm0, dword ptr[esi + 268h]
        subss   xmm0, ds : FLOAT_MIN_0_1
        jmp     short loc_114E54
        ; -------------------------------------------------------------------------- -

        loc_114E2F:; CODE XREF : CalcVibrationValues + 1AF
        ; CalcVibrationValues + 1B8
        movss   xmm5, dword ptr[esi + 264h]
        movss   xmm2, ds:FLOAT_MIN_0_1
        comiss  xmm5, xmm2
        jbe     short loc_114E96
        comiss  xmm1, ds : FLOAT_0_3
        jbe     short loc_114E96
        movaps  xmm0, xmm5
        subss   xmm0, xmm2

        loc_114E54 : ; CODE XREF : CalcVibrationValues + 1CD
        movss   xmm1, ds : FLOAT_2_0
        comiss  xmm0, xmm1
        jbe     short loc_114E64
        movaps  xmm0, xmm1

        loc_114E64 : ; CODE XREF : CalcVibrationValues + 1FF
        mulss   xmm0, dword ptr[esi + 1C4h]
        mulss   xmm0, ds : FLOAT_0_01
        addss   xmm0, xmm4
        movss   dword ptr[esp + 14h], xmm0
        mulss   xmm0, ds : FLOAT_3_0
        movss   dword ptr[esp + 10h], xmm0
        mov     edi, 0Ah
        jmp     loc_114F67
        ; -------------------------------------------------------------------------- -

        loc_114E96:; CODE XREF : CalcVibrationValues + 1E2
        ; CalcVibrationValues + 1EB
        movss   xmm2, ds:FLOAT_0_7
        comiss  xmm1, xmm2
        jbe     short loc_114F1F
        cmp[esp + 1Ch], ebp
        jz      short loc_114EFC
        movss   xmm0, dword ptr[esi + 1C4h]
        comiss  xmm0, ds:FLOAT_0_95
        jbe     short loc_114EDE
        movaps  xmm0, xmm1
        subss   xmm0, xmm2
        mulss   xmm0, dword ptr[esi + 1C4h]
        mulss   xmm0, ds : FLOAT_0_75
        movss   dword ptr[esp + 10h], xmm0
        mov     edi, 2
        jmp     short loc_114EE4
        ; -------------------------------------------------------------------------- -

        loc_114EDE:; CODE XREF : CalcVibrationValues + 258
        movss   xmm0, dword ptr[esp + 10h]

        loc_114EE4 : ; CODE XREF : CalcVibrationValues + 27C
        subss   xmm1, xmm2
        mulss   xmm1, xmm4
        mulss   xmm1, ds : FLOAT_5_0
        movss   dword ptr[esp + 14h], xmm1
        jmp     short loc_114F25
        ; -------------------------------------------------------------------------- -

        loc_114EFC:; CODE XREF : CalcVibrationValues + 247
        mulss   xmm0, ds : FLOAT_0_3
        mulss   xmm4, ds : FLOAT_0_75
        movss   dword ptr[esp + 10h], xmm0
        mov     edi, 0Ah
        movss   dword ptr[esp + 14h], xmm4
        jmp     short loc_114F67
        ; -------------------------------------------------------------------------- -

        loc_114F1F:; CODE XREF : CalcVibrationValues + 241
        movss   xmm0, dword ptr[esp + 10h]

        loc_114F25 : ; CODE XREF : CalcVibrationValues + 29A
        cmp     bx, bp
        jle     short loc_114FA8
        movsx   eax, bx
        cvtsi2ss xmm1, eax
        mulss   xmm1, ds : FLOAT_0_00001
        movaps  xmm2, xmm1
        mulss   xmm2, ds : FLOAT_2_0
        addss   xmm2, dword ptr[esp + 14h]
        mulss   xmm1, ds : FLOAT_1_5
        movss   dword ptr[esp + 14h], xmm2

        loc_114F58 : ; CODE XREF : CalcVibrationValues + 371
        addss   xmm1, xmm0
        movss   dword ptr[esp + 10h], xmm1
        mov     edi, 2

        loc_114F67 : ; CODE XREF : CalcVibrationValues + 231
        ; CalcVibrationValues + 2BD ...
        xorps   xmm1, xmm1

        loc_114F6A : ; CODE XREF : CalcVibrationValues + 3A2
        ; CalcVibrationValues + 3B0
        pop     ebx

        loc_114F6B : ; CODE XREF : CalcVibrationValues + AF
        ; CalcVibrationValues + 3BC
        mov     ecx, [esi + 8]
        mov     eax, ecx
        shr     eax, 6
        and eax, 1
        jz      loc_115021
        test    cl, cl
        js      loc_115021
        movss   xmm0, dword ptr[esi + 178h]
        mulss   xmm0, ds:FLOAT_0_5
        addss   xmm0, dword ptr[esp + 0Ch]
        movss   dword ptr[esp + 0Ch], xmm0
        add     edi, 14h
        jmp     loc_1150E1
        ; -------------------------------------------------------------------------- -

        loc_114FA8:; CODE XREF : CalcVibrationValues + 2C8
        test    dword ptr[esi + 4], 8000000h
        jz      short loc_114FD3
        movss   xmm1, dword ptr[esi + 1C4h]
        mulss   xmm1, xmm3
        comiss  xmm1, xmm3
        jbe     short loc_114F67
        movaps  xmm2, xmm1
        addss   xmm2, dword ptr[esp + 14h]
        movss   dword ptr[esp + 14h], xmm2
        jmp     short loc_114F58
        ; -------------------------------------------------------------------------- -

        loc_114FD3:; CODE XREF : CalcVibrationValues + 34F
        mov     eax, [esi + 8]
        test    ah, 1
        xorps   xmm1, xmm1
        jz      short loc_115007
        movss   xmm0, dword ptr[esi + 1C4h]
        mulss   xmm0, ds:FLOAT_10_0
        addss   xmm0, ds : FLOAT_1_0
        mulss   xmm0, dword ptr[esp + 14h]
        movss   dword ptr[esp + 14h], xmm0
        jmp     loc_114F6A
        ; -------------------------------------------------------------------------- -

        loc_115007:; CODE XREF : CalcVibrationValues + 37C
        xorps   xmm0, xmm0
        movss   dword ptr[esp + 14h], xmm0
        jmp     loc_114F6A
        ; -------------------------------------------------------------------------- -

        loc_115015:; CODE XREF : CalcVibrationValues + 88
        and dword ptr[esi + 8], 0FFFFFEBFh
        jmp     loc_114F6B
        ; -------------------------------------------------------------------------- -

        loc_115021:; CODE XREF : CalcVibrationValues + 316
        ; CalcVibrationValues + 31E
        cmp     eax, ebp
        jnz     short loc_115029
        test    cl, cl
        js      short loc_115061

        loc_115029 : ; CODE XREF : CalcVibrationValues + 3C3
        movss   xmm0, dword ptr[esi + 1DCh]
        ucomiss xmm0, xmm1
        lahf
        test    ah, 44h
        jnp     short loc_11504B
        movss   xmm0, dword ptr[esi + 2DCh]
        ucomiss xmm0, xmm1
        lahf
        test    ah, 44h
        jnp     short loc_115061

        loc_11504B : ; CODE XREF : CalcVibrationValues + 3D8
        comiss  xmm1, dword ptr[esi + 1E4h]
        jbe     short loc_115080
        movss   xmm0, dword ptr[esi + 1E0h]
        comiss  xmm0, xmm1
        jb      short loc_115080

        loc_115061 : ; CODE XREF : CalcVibrationValues + 3C7
        ; CalcVibrationValues + 3E9
        movss   xmm0, dword ptr[esp + 0Ch]
        addss   xmm0, ds:FLOAT_0_75
        movss   dword ptr[esp + 0Ch], xmm0
        add     edi, 0Fh
        mov     CalcVibrationValue_FrameCnt1, ebp
        jmp     short loc_1150E1
        ; -------------------------------------------------------------------------- -

        loc_115080:; CODE XREF : CalcVibrationValues + 3F2
        ; CalcVibrationValues + 3FF
        mov     eax, [esi + 4]
        test    eax, 20000h
        jz      loc_115144
        test    ch, 8
        jz      short loc_1150A9
        movss   xmm0, dword ptr[esp + 10h]
        addss   xmm0, dword ptr[esi + 1C4h]
        movss   dword ptr[esp + 10h], xmm0
        jmp     short loc_1150E1
        ; -------------------------------------------------------------------------- -

        loc_1150A9:; CODE XREF : CalcVibrationValues + 431
        fld     dword ptr[esp + 1Ch]
        movss   xmm0, dword ptr[esp + 0Ch]
        fmul    ds : FLOAT_10_0
        addss   xmm0, ds : FLOAT_0_75
        movss   dword ptr[esp + 0Ch], xmm0
        call    _ftol2
        add     edi, eax
        cmp     edi, 14h
        jbe     short loc_1150D8
        mov     edi, 14h

        loc_1150D8 : ; CODE XREF : CalcVibrationValues + 471
        ; CalcVibrationValues + 581
        mov     CalcVibrationValue_FrameCnt1, ebp

        loc_1150DE : ; CODE XREF : CalcVibrationValues + 567
        xorps   xmm1, xmm1

        loc_1150E1 : ; CODE XREF : CalcVibrationValues + 343
        ; CalcVibrationValues + 41E ...
        movss   xmm0, ds:FLOAT_0_01
        comiss  xmm0, dword ptr[esi + 1C4h]
        jbe     loc_11534C
        movss   xmm0, ds : FLOAT_0_001
        comiss  xmm0, dword ptr[esi + 1C4h]
        ja      short loc_11511D
        mov     eax, CalcVibrationValue_FrameCnt0
        inc     eax
        cmp     eax, 258h
        mov     CalcVibrationValue_FrameCnt0, eax
        jle     loc_115352

        loc_11511D : ; CODE XREF : CalcVibrationValues + 4A5
        push    ebp
        push    ebp
        movss   VibrationRightMotor, xmm1
        movss   VibrationLeftMotor, xmm1
        call    GetControllerId
        push    eax
        call    SetVibration
        add     esp, 0Ch
        pop     edi
        pop     esi
        pop     ebp
        add     esp, 18h
        retn
        ; -------------------------------------------------------------------------- -

        loc_115144:; CODE XREF : CalcVibrationValues + 428
        test    eax, 10000h
        jz      loc_1151E6
        test    ch, 10h
        jz      short loc_115175
        movss   xmm0, dword ptr[esi + 1C4h]
        mulss   xmm0, ds:FLOAT_0_5
        addss   xmm0, dword ptr[esp + 10h]
        movss   dword ptr[esp + 10h], xmm0
        jmp     loc_1150E1
        ; -------------------------------------------------------------------------- -

        loc_115175:; CODE XREF : CalcVibrationValues + 4F2
        mov     ecx, [esi + 0DD4h]
        imul    ecx, 3Ch
        mov     eax, 0x799B38[ecx] // s_EventWork[ecx].event_data
        cmp     eax, ebp
        jz      loc_1150E1
        movss   xmm0, dword ptr[eax + 1C4h]
        subss   xmm0, dword ptr[esi + 1C4h]
        mulss   xmm0, ds:FLOAT_25_0
        push    ecx
        movss   dword ptr[esp], xmm0; float
        call    Fabsf
        add     esp, 4
        call    _ftol2
        add     edi, eax
        cmp     edi, 14h
        jbe     short loc_1151C5
        mov     edi, 14h
        jmp     short loc_1151CD
        ; -------------------------------------------------------------------------- -

        loc_1151C5:; CODE XREF : CalcVibrationValues + 55C
        cmp     edi, ebp
        jbe     loc_1150DE

        loc_1151CD : ; CODE XREF : CalcVibrationValues + 563
        movss   xmm0, dword ptr[esp + 0Ch]
        addss   xmm0, ds : FLOAT_0_75
        movss   dword ptr[esp + 0Ch], xmm0
        jmp     loc_1150D8
        ; -------------------------------------------------------------------------- -

        loc_1151E6:; CODE XREF : CalcVibrationValues + 4E9
        test    ch, 20h
        jz      short loc_115211
        movss   xmm0, dword ptr[esi + 1C4h]
        mulss   xmm0, ds : FLOAT_0_5
        addss   xmm0, dword ptr[esp + 0Ch]
        movss   dword ptr[esp + 0Ch], xmm0
        mov     edi, 2
        jmp     loc_1150E1
        ; -------------------------------------------------------------------------- -

        loc_115211:; CODE XREF : CalcVibrationValues + 589
        cmp     edi, ebp
        jnz     loc_1150E1
        mov     edx, [esi + 208h]
        cmp     edx, [esi + 1D8h]
        jz      short loc_115245
        movss   xmm0, dword ptr[esp + 0Ch]
        addss   xmm0, ds:FLOAT_0_2
        movss   dword ptr[esp + 0Ch], xmm0
        mov     edi, 1
        jmp     loc_1150E1
        ; -------------------------------------------------------------------------- -

        loc_115245:; CODE XREF : CalcVibrationValues + 5C5
        movss   xmm0, dword ptr[esp + 1Ch]
        comiss  xmm0, ds : FLOAT_0_0018
        movss   xmm0, dword ptr[esp + 20h]
        jbe     short loc_1152A0
        comiss  xmm0, xmm1
        jb      short loc_115268
        comiss  xmm1, dword ptr[esi + 1D4h]
        ja      short loc_11527A

        loc_115268 : ; CODE XREF : CalcVibrationValues + 5FD
        comiss  xmm1, xmm0
        jbe     short loc_1152A0
        movss   xmm2, dword ptr[esi + 1D4h]
        comiss  xmm2, xmm1
        jb      short loc_1152A0

        loc_11527A : ; CODE XREF : CalcVibrationValues + 606
        movss   xmm0, dword ptr[esi + 1C4h]
        mulss   xmm0, ds : FLOAT_0_25
        addss   xmm0, dword ptr[esp + 0Ch]
        movss   dword ptr[esp + 0Ch], xmm0
        mov     edi, 2
        jmp     loc_1150E1
        ; -------------------------------------------------------------------------- -

        loc_1152A0:; CODE XREF : CalcVibrationValues + 5F8
        ; CalcVibrationValues + 60B ...
        comiss  xmm0, xmm1
        jbe     short loc_1152E5
        movss   xmm2, ds:FLOAT_0_33
        comiss  xmm2, dword ptr[esi + 1C4h]
        jbe     loc_1150E1
        comiss  xmm0, ds : FLOAT_0_001
        jbe     loc_1150E1
        movss   xmm0, dword ptr[esp + 0Ch]
        addss   xmm0, ds : FLOAT_0_3
        movss   dword ptr[esp + 0Ch], xmm0
        mov     edi, 0Ah
        jmp     loc_1150E1
        ; -------------------------------------------------------------------------- -

        loc_1152E5:; CODE XREF : CalcVibrationValues + 643
        movss   xmm2, ds : FLOAT_MIN_0_002
        comiss  xmm2, xmm0
        jbe     loc_1150E1
        movss   xmm2, ds : FLOAT_MIN_0_004
        comiss  xmm2, xmm0
        jbe     short loc_11532E
        cmp[esi + 38h], ebp
        jle     short loc_11532E
        movss   xmm0, dword ptr[esi + 1C4h]
        mulss   xmm0, ds:FLOAT_0_2
        addss   xmm0, dword ptr[esp + 0Ch]
        movss   dword ptr[esp + 0Ch], xmm0
        mov     edi, 2
        jmp     loc_1150E1
        ; -------------------------------------------------------------------------- -

        loc_11532E:; CODE XREF : CalcVibrationValues + 6A1
        ; CalcVibrationValues + 6A6
        movss   xmm0, dword ptr[esp + 0Ch]
        addss   xmm0, ds:FLOAT_0_2
        movss   dword ptr[esp + 0Ch], xmm0
        mov     edi, 2
        jmp     loc_1150E1
        ; -------------------------------------------------------------------------- -

        loc_11534C:; CODE XREF : CalcVibrationValues + 490
        mov     CalcVibrationValue_FrameCnt0, ebp

        loc_115352 : ; CODE XREF : CalcVibrationValues + 4B7
        movss   xmm1, dword ptr[esp + 10h]
        movss   xmm0, ds : FLOAT_0_5
        comiss  xmm1, xmm0
        jbe     short loc_11536B
        movss   dword ptr[esp + 10h], xmm0

        loc_11536B : ; CODE XREF : CalcVibrationValues + 703
        movss   xmm0, ds : FLOAT_0_00000011920929
        comiss  xmm0, dword ptr[esi + 1C4h]
        movss   xmm0, dword ptr[esp + 10h]
        movss   xmm2, ds : FLOAT_0_75
        comiss  xmm0, xmm2
        jbe     short loc_115390
        movaps  xmm0, xmm2

        loc_115390 : ; CODE XREF : CalcVibrationValues + 72B
        movss   xmm1, CalcVibrationValue_VibrationMult
        movaps  xmm3, xmm1
        mulss   xmm3, xmm0
        movss   xmm0, dword ptr[esp + 0Ch]
        comiss  xmm0, xmm2
        movss   VibrationRightMotor, xmm3
        jbe     short loc_1153B5
        movaps  xmm0, xmm2

        loc_1153B5 : ; CODE XREF : CalcVibrationValues + 750
        mov     eax, CalcVibrationValue_FrameCnt1
        cmp     eax, ebp
        jbe     short loc_1153CD
        dec     eax
        comiss  xmm0, VibrationLeftMotor
        mov     CalcVibrationValue_FrameCnt1, eax
        jbe     short loc_1153E9

        loc_1153CD : ; CODE XREF : CalcVibrationValues + 75C
        cmp     edi, 19h
        mulss   xmm1, xmm0
        movss   VibrationLeftMotor, xmm1
        jbe     short loc_1153E3
        mov     edi, 19h

        loc_1153E3 : ; CODE XREF : CalcVibrationValues + 77C
        mov     CalcVibrationValue_FrameCnt1, edi

        loc_1153E9 : ; CODE XREF : CalcVibrationValues + 76B
        pop     edi
        pop     esi
        pop     ebp
        add     esp, 18h
        retn
    }
}
