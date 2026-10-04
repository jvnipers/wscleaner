#include "plugin.h"
#include "addons.h"
#include "workshop_manager.h"
#include "filesystem.h"
#include "engine/IEngineService.h"
#include "icommandline.h"
#include "KeyValues.h"

CConVar<CUtlString> wscleaner_exclude("wscleaner_exclude", FCVAR_NONE, "Comma-separated list of addons that will not be deleted by the plugin.", "");

static void GetDownloadedAddonList(std::set<uint64> &outList)
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

// Remove the addon with the given ID from the workshop folder and from the acf file. The caller reloads Steam's workshop state afterwards.
static void RemoveAddon(uint64 addonID)
{
	if (!g_pFullFileSystem)
		return;
	ForgetWorkshopManagerMap(addonID);

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
	delete pACF;
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

static void GetWhitelistedAddons(std::set<uint64> &outList)
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

void CleanupWorkshopAddons()
{
	ISteamUGC *pUGC = g_SteamAPI.SteamUGC();
	if (!pUGC)
		return;

	std::set<uint64> downloadedAddons;
	GetDownloadedAddonList(downloadedAddons);

	// Steam refuses BInitWorkshopForGameServer while its workshop download job for the app is running, and that job
	// writes its in-memory item list back to the acf when it finishes, which would undo the cleanup. Retry next level.
	for (const auto &addonID : downloadedAddons)
	{
		if (pUGC->GetItemState(addonID) & (k_EItemStateDownloading | k_EItemStateDownloadPending))
		{
			META_CONPRINTF("[WSCleaner] Addon %llu is downloading, skipping cleanup this level.\n", addonID);
			return;
		}
	}

	std::set<uint64> whitelistedAddons;
	GetWhitelistedAddons(whitelistedAddons);
	bool removedAny = false;
	for (const auto &addonID : downloadedAddons)
	{
		if (whitelistedAddons.find(addonID) == whitelistedAddons.end())
		{
			RemoveAddon(addonID);
			removedAny = true;
		}
	}
	if (!removedAny)
		return;

	// Steam keeps its own copy of the acf in memory, which GetItemState and GetItemInstallInfo answer from, and the
	// workshop manager trusts those answers. Reload it once so Steam forgets the deleted items and downloads them
	// again when they are next requested.
	char szAbsolutePath[1024];
	V_MakeAbsolutePath(szAbsolutePath, sizeof(szAbsolutePath), ".\\steamapps\\workshop");
	if (!pUGC->BInitWorkshopForGameServer(730, szAbsolutePath))
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
