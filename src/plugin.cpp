#include <stdio.h>
#include <time.h>
#include <stdarg.h>
#include <vector>
#include "plugin.h"
#include "steam/steam_gameserver.h"
#include "filesystem.h"
#include "engine/IEngineService.h"
#include "icommandline.h"
#include "KeyValues.h"

SH_DECL_HOOK0_void(IServerGameDLL, GameServerSteamAPIActivated, SH_NOATTRIB, 0);
CSteamGameServerAPIContext g_SteamAPI;
WSCleanerPlugin g_ThisPlugin;
PLUGIN_EXPOSE(WSCleanerPlugin, g_ThisPlugin);
CConVar<CUtlString> wscleaner_exclude("wscleaner_exclude", FCVAR_NONE, "Comma-separated list of addons that will not be deleted by the plugin.", "");
CConVar<bool> wscleaner_debug("wscleaner_debug", FCVAR_NONE, "If enabled, log snapshots of the workshop content folder and appworkshop_730.acf before and after each cleanup pass.", false);

static void WSCleaner_GetWorkshopRoot(char *out, size_t outLen)
{
	V_MakeAbsolutePath(out, outLen, ".\\steamapps\\workshop");
}

static void WSCleaner_GetDebugLogPath(char *out, size_t outLen)
{
	V_MakeAbsolutePath(out, outLen, ".\\logs\\wscleaner_debug.log");
}

// Print a line to the server console AND, if pLog is non-null, append it to the debug log file.
static void WSCleaner_DebugPrintf(FILE *pLog, const char *fmt, ...)
{
	char buffer[4096];
	va_list args;
	va_start(args, fmt);
	V_vsnprintf(buffer, sizeof(buffer), fmt, args);
	va_end(args);

	META_CONPRINTF("%s", buffer);
	if (pLog)
	{
		fputs(buffer, pLog);
	}
}

static FILE *WSCleaner_OpenDebugLog()
{
	char szLogPath[1024];
	WSCleaner_GetDebugLogPath(szLogPath, sizeof(szLogPath));

	// Ensure the logs directory exists.
	if (g_pFullFileSystem)
	{
		char szLogsDir[1024];
		V_MakeAbsolutePath(szLogsDir, sizeof(szLogsDir), ".\\logs");
		g_pFullFileSystem->CreateDirHierarchy(szLogsDir, "DEFAULT_WRITE_PATH");
	}

	FILE *pLog = fopen(szLogPath, "ab");
	if (!pLog)
	{
		META_CONPRINTF("[WSCleaner][debug] WARNING: could not open debug log file '%s' for append.\n", szLogPath);
	}
	return pLog;
}

// Dump the list of subfolders under steamapps/workshop/content/730 with a header tag.
static void WSCleaner_DumpContentFolderSnapshot(FILE *pLog, const char *tag)
{
	if (!g_pFullFileSystem)
		return;

	char szWorkshop[1024];
	WSCleaner_GetWorkshopRoot(szWorkshop, sizeof(szWorkshop));
	std::string contentDir = std::string(szWorkshop) + "/content/730";
	std::string searchPath = contentDir + "/*";

	WSCleaner_DebugPrintf(pLog, "[WSCleaner][debug] --- content folder snapshot (%s) : %s ---\n", tag, contentDir.c_str());

	FileFindHandle_t findHandle = {};
	const char *fileName = g_pFullFileSystem->FindFirstEx(searchPath.c_str(), "GAME", &findHandle);
	int count = 0;
	while (fileName)
	{
		if (g_pFullFileSystem->FindIsDirectory(findHandle)
			&& V_strcmp(fileName, ".") != 0
			&& V_strcmp(fileName, "..") != 0)
		{
			WSCleaner_DebugPrintf(pLog, "[WSCleaner][debug]   %s\n", fileName);
			++count;
		}
		fileName = g_pFullFileSystem->FindNext(findHandle);
	}
	g_pFullFileSystem->FindClose(findHandle);

	WSCleaner_DebugPrintf(pLog, "[WSCleaner][debug] --- end content folder snapshot (%s) : %d entries ---\n", tag, count);
}

// Dump the raw contents of appworkshop_730.acf with a header tag.
static void WSCleaner_DumpACFSnapshot(FILE *pLog, const char *tag)
{
	if (!g_pFullFileSystem)
		return;

	char szWorkshop[1024];
	WSCleaner_GetWorkshopRoot(szWorkshop, sizeof(szWorkshop));
	std::string acfPath = std::string(szWorkshop) + "/appworkshop_730.acf";

	WSCleaner_DebugPrintf(pLog, "[WSCleaner][debug] --- acf snapshot (%s) : %s ---\n", tag, acfPath.c_str());

	FileHandle_t hFile = g_pFullFileSystem->Open(acfPath.c_str(), "rb", "GAME");
	if (!hFile)
	{
		WSCleaner_DebugPrintf(pLog, "[WSCleaner][debug]   (acf not found or could not be opened)\n");
		WSCleaner_DebugPrintf(pLog, "[WSCleaner][debug] --- end acf snapshot (%s) ---\n", tag);
		return;
	}

	unsigned int size = g_pFullFileSystem->Size(hFile);
	std::string buffer;
	buffer.resize(size + 1);
	g_pFullFileSystem->Read(&buffer[0], size, hFile);
	g_pFullFileSystem->Close(hFile);
	buffer[size] = '\0';

	// Print line-by-line so individual lines aren't truncated by console buffer limits.
	size_t start = 0;
	for (size_t i = 0; i <= size; ++i)
	{
		if (i == size || buffer[i] == '\n')
		{
			size_t end = i;
			if (end > start && buffer[end - 1] == '\r')
				--end;
			std::string line = buffer.substr(start, end - start);
			WSCleaner_DebugPrintf(pLog, "[WSCleaner][debug]   %s\n", line.c_str());
			start = i + 1;
		}
	}

	WSCleaner_DebugPrintf(pLog, "[WSCleaner][debug] --- end acf snapshot (%s) : %u bytes ---\n", tag, size);
}

static void WSCleaner_DumpSnapshots(const char *tag)
{
	if (!wscleaner_debug.Get())
		return;

	FILE *pLog = WSCleaner_OpenDebugLog();

	time_t now = time(nullptr);
	struct tm tmBuf;
#ifdef _WIN32
	localtime_s(&tmBuf, &now);
#else
	localtime_r(&now, &tmBuf);
#endif
	char szTimestamp[64];
	strftime(szTimestamp, sizeof(szTimestamp), "%Y-%m-%d %H:%M:%S", &tmBuf);
	WSCleaner_DebugPrintf(pLog, "[WSCleaner][debug] ===== %s : %s =====\n", szTimestamp, tag);

	WSCleaner_DumpContentFolderSnapshot(pLog, tag);
	WSCleaner_DumpACFSnapshot(pLog, tag);

	if (pLog)
		fclose(pLog);
}

void GetDownloadedAddonList(std::set<uint64> &outList)
{
	outList.clear();
	if (!g_pFullFileSystem)
		return;
	
	char szAbsolutePath[1024];
	V_MakeAbsolutePath(szAbsolutePath, sizeof(szAbsolutePath), ".\\steamapps\\workshop\\content\\730");
	// Loop through all the directories in the workshop folder
	std::string searchPath = std::string(szAbsolutePath) + "/*";
	FileFindHandle_t findHandle = {};
	const char *fileName = g_pFullFileSystem->FindFirstEx(searchPath.c_str(), "GAME", &findHandle);
	while (fileName)
	{
		if (g_pFullFileSystem->FindIsDirectory(findHandle))
		{
			uint64 addonID = strtoull(fileName, nullptr, 10);
			if (addonID != 0)
			{
				outList.insert(addonID);
			}
		}
		fileName = g_pFullFileSystem->FindNext(findHandle);
	}
	g_pFullFileSystem->FindClose(findHandle);
}

// Walk the ACF's WorkshopItemsInstalled / WorkshopItemDetails sections and remove any
// entry whose corresponding directory under content/730/<id> does not exist on disk.
// Returns the number of stale entries pruned. Stale entries cause the engine to try to
// mount addons whose files aren't actually there, which falls back to the 'error' map.
int PruneStaleACFEntries()
{
	if (!g_pFullFileSystem)
		return 0;

	char szWorkshop[1024];
	V_MakeAbsolutePath(szWorkshop, sizeof(szWorkshop), ".\\steamapps\\workshop");
	std::string acfPath = std::string(szWorkshop) + "/appworkshop_730.acf";
	std::string contentRoot = std::string(szWorkshop) + "/content/730";

	KeyValues *pACF = new KeyValues("AppWorkshop");
	KeyValues::AutoDelete autoDelete(pACF);
	if (!pACF->LoadFromFile(g_pFullFileSystem, acfPath.c_str(), "GAME"))
		return 0;

	auto pruneSection = [&](const char *sectionName) -> int
	{
		KeyValues *pSection = pACF->FindKey(sectionName);
		if (!pSection)
			return 0;
		int pruned = 0;
		// Collect candidate IDs first so we can safely delete while iterating.
		std::vector<std::string> stale;
		for (KeyValues *pItem = pSection->GetFirstSubKey(); pItem; pItem = pItem->GetNextKey())
		{
			const char *idStr = pItem->GetName();
			if (!idStr || idStr[0] == '\0')
				continue;
			uint64 id = strtoull(idStr, nullptr, 10);
			if (id == 0)
				continue;
			std::string folder = contentRoot + "/" + idStr;
			if (!g_pFullFileSystem->IsDirectory(folder.c_str(), "GAME"))
				stale.push_back(idStr);
		}
		for (const auto &idStr : stale)
		{
			if (pSection->FindAndDeleteSubKey(idStr.c_str()))
			{
				META_CONPRINTF("[WSCleaner] Pruned stale ACF entry from %s: %s (folder missing)\n", sectionName, idStr.c_str());
				++pruned;
			}
		}
		return pruned;
	};

	int totalPruned = 0;
	totalPruned += pruneSection("WorkshopItemsInstalled");
	totalPruned += pruneSection("WorkshopItemDetails");

	if (totalPruned > 0)
	{
		pACF->SaveToFile(g_pFullFileSystem, acfPath.c_str(), "GAME");
		if (g_SteamAPI.SteamUGC())
			g_SteamAPI.SteamUGC()->BInitWorkshopForGameServer(730, szWorkshop);
	}
	return totalPruned;
}

// Remove the addon with the given ID from the workshop folder, update the acf file accordingly, and reload the workshop items.
void RemoveAddon(uint64 addonID)
{
	if (!g_pFullFileSystem || !g_SteamAPI.SteamUGC())
		return;
	char szAbsolutePath[1024];
	V_MakeAbsolutePath(szAbsolutePath, sizeof(szAbsolutePath), ".\\steamapps\\workshop");
	char addonIDStr[32];
	V_snprintf(addonIDStr, sizeof(addonIDStr), "%llu", addonID);
	std::string fullPath = std::string(szAbsolutePath) + "/content/730/" + addonIDStr;
	if (g_pFullFileSystem->IsDirectory(fullPath.c_str(), "GAME"))
	{
		if (g_pFullFileSystem->DeleteDirectoryAndContents_R(fullPath.c_str(), "GAME", true))
		{
			META_CONPRINTF("[WSCleaner] Removed addon: %llu\n", addonID);
		}
		else
		{
			META_CONPRINTF("[WSCleaner] Failed to remove addon: %llu\n", addonID);
		}
	}

	// Now update the acf file to remove the entry for this addon
	KeyValues *pACF = new KeyValues("AppWorkshop");
	std::string acfPath = std::string(szAbsolutePath) + "/appworkshop_730.acf";
	if (pACF->LoadFromFile(g_pFullFileSystem, acfPath.c_str(), "GAME"))
	{
		KeyValues *pInstalledItems = pACF->FindKey("WorkshopItemsInstalled");
		if (pInstalledItems && pInstalledItems->FindAndDeleteSubKey(addonIDStr))
		{
			META_CONPRINTF("[WSCleaner] Removed entry from ACF for addon: %llu\n", addonID);
		}
		KeyValues *pItemDetails = pACF->FindKey("WorkshopItemDetails");
		if (pItemDetails && pItemDetails->FindAndDeleteSubKey(addonIDStr))
		{
			META_CONPRINTF("[WSCleaner] Removed details from ACF for addon: %llu\n", addonID);
		}
		pACF->SaveToFile(g_pFullFileSystem, acfPath.c_str(), "GAME");
	}
	g_SteamAPI.SteamUGC()->BInitWorkshopForGameServer(730, szAbsolutePath);
}

void ExtractValuesAfterKeyword(const std::string& str, const std::string& keyword, std::set<std::string>& results)
{
	size_t pos = 0;
	
	while ((pos = str.find(keyword, pos)) != std::string::npos) {
		// Check if keyword is followed by whitespace (word boundary)
		size_t afterKeyword = pos + keyword.length();
		if (afterKeyword < str.length() && !std::isspace(str[afterKeyword])) {
			pos++;
			continue;
		}
		
		pos = afterKeyword;
		
		// Skip whitespace after keyword
		while (pos < str.length() && std::isspace(str[pos])) {
			pos++;
		}
		
		if (pos >= str.length()) break;
		
		std::string value;
		
		// Check if value is quoted
		if (str[pos] == '"') {
			pos++; // Skip opening quote
			size_t endQuote = str.find('"', pos);
			if (endQuote != std::string::npos) {
				value = str.substr(pos, endQuote - pos);
				pos = endQuote + 1;
			}
		} else {
			// Extract until whitespace
			size_t start = pos;
			while (pos < str.length() && !std::isspace(str[pos])) {
				pos++;
			}
			value = str.substr(start, pos - start);
		}
		
		if (!value.empty()) {
			results.insert(value);
		}
	}
}

void GetWhitelistedAddons(std::set<uint64> &outList)
{
	outList.clear();
	// Do not remove currently loaded addons.
	int numAddons = g_pEngineServiceMgr->GetAddonCount();
	for (int i = 0; i < numAddons; ++i)
	{
		const char *addonName = g_pEngineServiceMgr->GetAddon(i);
		if (addonName && addonName[0] != '\0')
		{
			uint64 addonID = strtoull(addonName, nullptr, 10);
			if (addonID != 0)
			{
				outList.insert(addonID);
			}
		}
	}

	std::set<std::string> stringResults;
	CSplitString ss(wscleaner_exclude.Get(), ",");
	ExtractValuesAfterKeyword(CommandLine()->GetCmdLine(), "+host_workshop_map", stringResults);
	for (int i = 0; i < ss.Count(); ++i)
	{
		stringResults.insert(std::string(ss[i]));
	}
	// Convert strings to uint64
	for (const auto &str : stringResults)
	{
		uint64 addonID = strtoull(str.c_str(), nullptr, 10);
		if (addonID != 0)
		{
			outList.insert(addonID);
		}
	}
}


bool WSCleanerPlugin::Load(PluginId id, ISmmAPI *ismm, char *error, size_t maxlen, bool late)
{
	PLUGIN_SAVEVARS();
	// This should not run if the server is not dedicated.
	if (!CommandLine()->CheckParm("-dedicated"))
	{
		snprintf(error, maxlen, "This plugin can only be run on dedicated servers.");
		return false;
	}
	GET_V_IFACE_CURRENT(GetEngineFactory, g_pCVar, ICvar, CVAR_INTERFACE_VERSION);
	GET_V_IFACE_CURRENT(GetEngineFactory, g_pEngineServer, IVEngineServer2, INTERFACEVERSION_VENGINESERVER);
	GET_V_IFACE_ANY(GetServerFactory, g_pSource2Server, ISource2Server, SOURCE2SERVER_INTERFACE_VERSION);
	GET_V_IFACE_ANY(GetFileSystemFactory, g_pFullFileSystem, IFileSystem, FILESYSTEM_INTERFACE_VERSION);
	GET_V_IFACE_ANY(GetEngineFactory, g_pEngineServiceMgr, IEngineServiceMgr, ENGINESERVICEMGR_INTERFACE_VERSION);
	g_SMAPI->AddListener( this, this );
	if (late)
		g_SteamAPI.Init();
	ConVar_Register();

	SH_ADD_HOOK(IServerGameDLL, GameServerSteamAPIActivated, g_pSource2Server, SH_MEMBER(this, &WSCleanerPlugin::Hook_GameServerSteamAPIActivated), false);
	return true;
}

bool WSCleanerPlugin::Unload(char *error, size_t maxlen)
{
	SH_REMOVE_HOOK(IServerGameDLL, GameServerSteamAPIActivated, g_pSource2Server, SH_MEMBER(this, &WSCleanerPlugin::Hook_GameServerSteamAPIActivated), false);
	return true;
}

void WSCleanerPlugin::AllPluginsLoaded()
{
	g_pEngineServer->ServerCommand("exec wscleaner/wscleaner");
}

void WSCleanerPlugin::OnLevelInit(char const *pMapName, 
							char const *pMapEntities, 
							char const *pOldLevel, 
							char const *pLandmarkName, 
							bool loadGame, 
							bool background) 
{
	// Always reconcile the ACF against the on-disk content folder. If a prior cleanup,
	// Steam container restart, or external file deletion left the ACF claiming an addon
	// is installed when its folder is missing, the engine will try to mount it on the
	// next changelevel, fail to find any maps, and fall back to the 'error' map.
	PruneStaleACFEntries();

	DoCleanup();
}

void WSCleanerPlugin::DoCleanup()
{
	// Delete every downloaded addon that is not currently mounted and not in the
	// wscleaner_exclude / +host_workshop_map whitelist. The ACF prune step in
	// OnLevelInit (and the wscleaner_prune_acf command) keeps the ACF in sync with
	// disk so the engine won't try to mount addons we have removed.
	std::set<uint64> protectedAddons;
	GetWhitelistedAddons(protectedAddons);

	// Safety: if the engine reports no mounted addons right now, the addon list is
	// unreliable (e.g. we are mid-transition) -- refuse to delete to avoid wiping
	// content the engine is about to mount for the next map.
	if (protectedAddons.empty())
	{
		META_CONPRINTF("[WSCleaner] Skipping cleanup: no mounted addons reported by the engine.\n");
		return;
	}

	std::set<uint64> downloadedAddons;
	GetDownloadedAddonList(downloadedAddons);

	bool willRemoveAny = false;
	for (const auto &addonID : downloadedAddons)
	{
		if (protectedAddons.find(addonID) == protectedAddons.end())
		{
			willRemoveAny = true;
			break;
		}
	}
	if (!willRemoveAny)
		return;

	WSCleaner_DumpSnapshots("before cleanup");
	for (const auto &addonID : downloadedAddons)
	{
		if (protectedAddons.find(addonID) == protectedAddons.end())
		{
			RemoveAddon(addonID);
		}
	}
	WSCleaner_DumpSnapshots("after cleanup");
}

void WSCleanerPlugin::Hook_GameServerSteamAPIActivated()
{
	if (g_SteamAPI.SteamUGC())
		return;
	g_SteamAPI.Init();
}

CON_COMMAND_F(wscleaner_clean, "Manually clean unused workshop addons now.", FCVAR_NONE)
{
	g_ThisPlugin.DoCleanup();
}

CON_COMMAND_F(wscleaner_prune_acf, "Remove ACF entries for workshop addons whose folder is missing on disk.", FCVAR_NONE)
{
	int pruned = PruneStaleACFEntries();
	META_CONPRINTF("[WSCleaner] ACF prune complete. %d stale entries removed.\n", pruned);
}

CON_COMMAND_F(wscleaner_exclude_add, "Add an addon to the addon whitelist", FCVAR_NONE)
{
	if (args.ArgC() < 2)
	{
		META_CONPRINTF("Usage: wscleaner_exclude_add <ID>\n");
		return;
	}
	uint64 addonID = strtoull(args[1], nullptr, 10);
	if (addonID == 0)
	{
		META_CONPRINTF("Invalid addon ID: %s\n", args[1]);
		return;
	}
	if (wscleaner_exclude.Get()[0] == '\0')
	{
		wscleaner_exclude.Set(args[1]);
		return;
	}
	wscleaner_exclude.Set(wscleaner_exclude.Get() + "," + args[1]);
}

CON_COMMAND_F(wscleaner_exclude_remove, "Remove an addon from the addon whitelist", FCVAR_NONE)
{
	if (args.ArgC() < 2)
	{
		META_CONPRINTF("Usage: wscleaner_exclude_remove <ID>\n");
		return;
	}
	uint64 addonID = strtoull(args[1], nullptr, 10);
	if (addonID == 0)
	{
		META_CONPRINTF("Invalid addon ID: %s\n", args[1]);
		return;
	}
	std::set<std::string> currentIDs;
	CSplitString ss(wscleaner_exclude.Get(), ",");
	for (int i = 0; i < ss.Count(); ++i)
	{
		currentIDs.insert(std::string(ss[i]));
	}
	std::string idStr = std::to_string(addonID);
	if (currentIDs.erase(idStr) > 0)
	{
		// Rebuild the convar value
		std::string newValue;
		for (const auto &id : currentIDs)
		{
			if (!newValue.empty())
				newValue += ",";
			newValue += id;
		}
		wscleaner_exclude.Set(newValue.c_str());
	}
	else
	{
		META_CONPRINTF("Addon ID %llu not found in whitelist.\n", addonID);
	}
}