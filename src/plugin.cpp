#include <stdio.h>
#include "plugin.h"
#include "addons.h"
#include "filesystem.h"
#include "engine/IEngineService.h"
#include "icommandline.h"

CSteamGameServerAPIContext g_SteamAPI;
WSCleanerPlugin g_ThisPlugin;
PLUGIN_EXPOSE(WSCleanerPlugin, g_ThisPlugin);

WSCleanerPlugin::WSCleanerPlugin() :
	m_GameServerSteamAPIActivated(&IServerGameDLL::GameServerSteamAPIActivated, this, &WSCleanerPlugin::Hook_GameServerSteamAPIActivated, nullptr)
{
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

	m_GameServerSteamAPIActivated.Add(g_pSource2Server);
	return true;
}

bool WSCleanerPlugin::Unload(char *error, size_t maxlen)
{
	m_GameServerSteamAPIActivated.Remove(g_pSource2Server);
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
	CleanupWorkshopAddons();
}

KHook::Return<void> WSCleanerPlugin::Hook_GameServerSteamAPIActivated(IServerGameDLL*)
{
	if (!g_SteamAPI.SteamUGC())
		g_SteamAPI.Init();
	return { KHook::Action::Ignore };
}
