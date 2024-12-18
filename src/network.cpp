#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")
#include <string>
#include "resource.h"

namespace Util {

std::string HttpGetRequest(const std::wstring& host, const std::wstring& path, bool https = true)
{
	HINTERNET hSession = WinHttpOpen(L"OutRun2006Tweaks/1.0",
		WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
		WINHTTP_NO_PROXY_NAME,
		WINHTTP_NO_PROXY_BYPASS, 0);
	if (!hSession)
		return "";

	HINTERNET hConnect = WinHttpConnect(hSession, host.c_str(), https ? INTERNET_DEFAULT_HTTPS_PORT : INTERNET_DEFAULT_HTTP_PORT, 0);
	if (!hConnect)
	{
		WinHttpCloseHandle(hSession);
		return "";
	}

	HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", path.c_str(),
		NULL, WINHTTP_NO_REFERER,
		WINHTTP_DEFAULT_ACCEPT_TYPES,
		https ? WINHTTP_FLAG_SECURE : 0);
	if (!hRequest)
	{
		WinHttpCloseHandle(hConnect);
		WinHttpCloseHandle(hSession);
		return "";
	}

	BOOL bResults = WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
		WINHTTP_NO_REQUEST_DATA, 0, 0, 0);

	if (bResults)
		bResults = WinHttpReceiveResponse(hRequest, NULL);

	std::string response;
	if (bResults)
	{
		DWORD dwSize = 0;
		do
		{
			DWORD dwDownloaded = 0;
			if (!WinHttpQueryDataAvailable(hRequest, &dwSize))
				break;

			if (dwSize == 0)
				break;

			char* buffer = new char[dwSize + 1];
			if (!WinHttpReadData(hRequest, buffer, dwSize, &dwDownloaded))
			{
				delete[] buffer;
				break;
			}

			buffer[dwDownloaded] = '\0';
			response.append(buffer, dwDownloaded);
			delete[] buffer;
		} while (dwSize > 0);
	}

	WinHttpCloseHandle(hRequest);
	WinHttpCloseHandle(hConnect);
	WinHttpCloseHandle(hSession);

	return response;
}

};