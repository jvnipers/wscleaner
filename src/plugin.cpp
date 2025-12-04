#include <stdio.h>
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
	std::set<uint64> downloadedAddons;
	GetDownloadedAddonList(downloadedAddons);
	std::set<uint64> whitelistedAddons;
	GetWhitelistedAddons(whitelistedAddons);
	for (const auto &addonID : downloadedAddons)
	{
		if (whitelistedAddons.find(addonID) == whitelistedAddons.end())
		{
			RemoveAddon(addonID);
		}
	}
}

void WSCleanerPlugin::Hook_GameServerSteamAPIActivated()
{
	if (g_SteamAPI.SteamUGC())
		return;
	g_SteamAPI.Init();
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