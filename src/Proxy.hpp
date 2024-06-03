#pragma once

namespace proxy
{
extern HMODULE origModule;
bool on_attach(HMODULE ourModule);
void on_detach();
};

#define PLUGIN_API extern "C" __declspec(dllexport)
