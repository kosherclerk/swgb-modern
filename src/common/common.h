// SWGB Modern - shared helpers (config + logging) for ddraw.dll and dsound.dll
#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string>

namespace swgbm {

// Directory of the game (where the exe lives); config file lives here.
const std::wstring& GameDir();
std::wstring ConfigPath();     // <GameDir>\SWGBModern.ini

int         CfgInt(const char* section, const char* key, int def);
float       CfgFloat(const char* section, const char* key, float def);
std::string CfgStr(const char* section, const char* key, const char* def);
bool        CfgBool(const char* section, const char* key, bool def);

void Log(const char* fmt, ...);
// [Debug] HeapCheck: validate all process heaps, log the first failing checkpoint
void HeapCheckpoint(const char* where);
bool LogEnabled();

// Identification of the host executable so engine patches are only applied
// to binaries we know byte-for-byte.
enum class GameExe { Unknown, BaseGame, CloneCampaigns, ExpandingFronts };
GameExe     DetectExe();
const char* ExeName(GameExe e);

} // namespace swgbm

#define LOG(...) do { if (swgbm::LogEnabled()) swgbm::Log(__VA_ARGS__); } while (0)
