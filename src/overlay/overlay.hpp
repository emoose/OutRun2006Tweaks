#pragma once

namespace Overlay
{
	inline float GlobalFontScale = 1.5f;

	inline int NotifyDisplayTime = 7;
	inline bool NotifyOnlineEnable = true;
	inline int NotifyOnlineUpdateTime = 20;
	inline int NotifyHideMode = 1;
	inline bool NotifyUpdateCheck = true;

	enum NotifyHideModes
	{
		NotifyHideMode_Never = 0,
		NotifyHideMode_OnlineRaces = 1,
		NotifyHideMode_AllRaces = 2
	};

	inline bool CourseReplacementEnabled = false;
	inline char CourseReplacementCode[256] = { 0 };

	bool settings_read();
	bool settings_write();
};