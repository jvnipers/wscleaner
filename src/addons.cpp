#include "plugin.h"
#include "addons.h"
#include "workshop_manager.h"
#include "filesystem.h"
#include "engine/IEngineService.h"
#include "icommandline.h"
#include "KeyValues.h"

CConVar<CUtlString> wscleaner_exclude("wscleaner_exclude", FCVAR_NONE, "Comma-separated list of addons that will not be deleted by the plugin.", "");

static std::string GetWorkshopRoot()
{
	char szAbsolutePath[1024];
	V_MakeAbsolutePath(szAbsolutePath, sizeof(szAbsolutePath), ".\\steamapps\\workshop");
	return szAbsolutePath;
}

static std::string GetAddonFolder(uint64 addonID)
{
	return GetWorkshopRoot() + "/content/730/" + std::to_string(addonID);
}

static void GetDownloadedAddonList(std::set<uint64> &outList)
{
	outList.clear();
	if (!g_pFullFileSystem)
		return;

	// Loop through all the directories in the workshop folder
	std::string searchPath = GetWorkshopRoot() + "/content/730/*";
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

// Whether the addon's folder holds a vpk the engine could mount.
static bool AddonFolderHasVPK(uint64 addonID)
{
	std::string searchPath = GetAddonFolder(addonID) + "/*.vpk";
	FileFindHandle_t findHandle = {};
	bool found = g_pFullFileSystem->FindFirstEx(searchPath.c_str(), "GAME", &findHandle) != nullptr;
	g_pFullFileSystem->FindClose(findHandle);
	return found;
}

static bool DeleteAddonFolder(uint64 addonID)
{
	std::string folder = GetAddonFolder(addonID);
	if (!g_pFullFileSystem->IsDirectory(folder.c_str(), "GAME"))
		return false;
	if (!g_pFullFileSystem->DeleteDirectoryAndContents_R(folder.c_str(), "GAME", true))
	{
		META_CONPRINTF("[WSCleaner] Failed to remove addon: %llu\n", addonID);
		return false;
	}
	META_CONPRINTF("[WSCleaner] Removed addon: %llu\n", addonID);
	return true;
}

static std::string GetACFPath()
{
	return GetWorkshopRoot() + "/appworkshop_730.acf";
}

// Addons Steam's acf lists as installed.
static void GetACFInstalledAddons(std::set<uint64> &outList)
{
	KeyValues::AutoDelete pACF("AppWorkshop");
	if (!pACF->LoadFromFile(g_pFullFileSystem, GetACFPath().c_str(), "GAME"))
		return;
	KeyValues *pInstalledItems = pACF->FindKey("WorkshopItemsInstalled");
	if (!pInstalledItems)
		return;
	for (KeyValues *pItem = pInstalledItems->GetFirstSubKey(); pItem; pItem = pItem->GetNextKey())
	{
		uint64 addonID = strtoull(pItem->GetName(), nullptr, 10);
		if (addonID != 0)
			outList.insert(addonID);
	}
}

// Drop the addons from the acf. Returns whether the file changed.
static bool PruneACF(const std::set<uint64> &addons)
{
	if (addons.empty())
		return false;
	KeyValues::AutoDelete pACF("AppWorkshop");
	std::string acfPath = GetACFPath();
	if (!pACF->LoadFromFile(g_pFullFileSystem, acfPath.c_str(), "GAME"))
		return false;
	bool changed = false;
	for (uint64 addonID : addons)
	{
		std::string addonIDStr = std::to_string(addonID);
		KeyValues *pInstalledItems = pACF->FindKey("WorkshopItemsInstalled");
		if (pInstalledItems && pInstalledItems->FindAndDeleteSubKey(addonIDStr.c_str()))
		{
			META_CONPRINTF("[WSCleaner] Removed entry from ACF for addon: %llu\n", addonID);
			changed = true;
		}
		KeyValues *pItemDetails = pACF->FindKey("WorkshopItemDetails");
		if (pItemDetails && pItemDetails->FindAndDeleteSubKey(addonIDStr.c_str()))
		{
			META_CONPRINTF("[WSCleaner] Removed details from ACF for addon: %llu\n", addonID);
			changed = true;
		}
	}
	if (changed)
		pACF->SaveToFile(g_pFullFileSystem, acfPath.c_str(), "GAME");
	return changed;
}

static void ExtractValuesAfterKeyword(const std::string& str, const std::string& keyword, std::set<std::string>& results)
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

static void GetWhitelistedAddons(const char *currentMap, std::set<uint64> &outList)
{
	outList.clear();
	// The addon the running map comes from, in case it is missing from the engine's addon list.
	uint64 currentMapAddon = FindWorkshopManagerMapAddon(currentMap);
	if (currentMapAddon != 0)
		outList.insert(currentMapAddon);

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

	GetWorkshopManagerBusyAddons(outList);

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

void CleanupWorkshopAddons(const char *currentMap)
{
	ISteamUGC *pUGC = g_SteamAPI.SteamUGC();
	if (!pUGC || !g_pFullFileSystem)
		return;

	std::set<uint64> downloadedAddons;
	GetDownloadedAddonList(downloadedAddons);
	// Everything something believes is installed: Steam's acf and the workshop manager's loaded maps.
	std::set<uint64> listedAddons;
	GetACFInstalledAddons(listedAddons);
	GetWorkshopManagerLoadedAddons(listedAddons);

	// Steam refuses BInitWorkshopForGameServer while its workshop download job for the app is running, and that job
	// writes its in-memory item list back to the acf when it finishes, which would undo the cleanup. Retry next level.
	std::set<uint64> knownAddons = downloadedAddons;
	knownAddons.insert(listedAddons.begin(), listedAddons.end());
	for (const auto &addonID : knownAddons)
	{
		if (pUGC->GetItemState(addonID) & (k_EItemStateDownloading | k_EItemStateDownloadPending))
		{
			META_CONPRINTF("[WSCleaner] Addon %llu is downloading, skipping cleanup this level.\n", addonID);
			return;
		}
	}

	std::set<uint64> whitelistedAddons;
	GetWhitelistedAddons(currentMap, whitelistedAddons);
	std::set<uint64> droppedAddons;
	for (const auto &addonID : downloadedAddons)
	{
		if (whitelistedAddons.find(addonID) == whitelistedAddons.end())
		{
			DeleteAddonFolder(addonID);
			droppedAddons.insert(addonID);
		}
	}

	// Repair addons that are listed as installed but have nothing mountable on disk, for example after the server
	// died halfway through a cleanup. Left alone, Steam keeps reporting them as installed, the workshop manager skips
	// the download, and hosting them lands on the error map. Only a request the manager is still working on is spared.
	for (const auto &addonID : knownAddons)
	{
		if (droppedAddons.count(addonID) || IsWorkshopManagerRequestPending(addonID) || AddonFolderHasVPK(addonID))
			continue;
		META_CONPRINTF("[WSCleaner] Addon %llu is listed as installed but has no files, dropping it.\n", addonID);
		DeleteAddonFolder(addonID);
		droppedAddons.insert(addonID);
	}
	if (droppedAddons.empty())
		return;

	for (const auto &addonID : droppedAddons)
		ForgetWorkshopManagerMap(addonID);
	PruneACF(droppedAddons);

	// Steam keeps its own copy of the acf in memory, which GetItemState and GetItemInstallInfo answer from, and the
	// workshop manager trusts those answers. Reload it once so Steam forgets the dropped items and downloads them
	// again when they are next requested.
	if (!pUGC->BInitWorkshopForGameServer(730, GetWorkshopRoot().c_str()))
		META_CONPRINTF("[WSCleaner] Steam refused to reload workshop state, removed addons may still be reported as installed.\n");
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
