#ifndef _INCLUDE_WSHOSTMAP_PLUGIN_H_
#define _INCLUDE_WSHOSTMAP_PLUGIN_H_

#include <ISmmPlugin.h>
#include "version_gen.h"
#include "icvar.h"
#include "steam/steam_gameserver.h"

// Optional companion plugin: lets anyone type "!hostmap <id>" in chat to host a workshop map, the same way
// the host_workshop_map console command does, with the item's details and download progress shown in chat.
class WSHostMapPlugin : public ISmmPlugin, public IMetamodListener
{
public:
	WSHostMapPlugin();

	bool Load(PluginId id, ISmmAPI *ismm, char *error, size_t maxlen, bool late);
	bool Unload(char *error, size_t maxlen);
	void AllPluginsLoaded();

	KHook::Return<void> Hook_GameServerSteamAPIActivated(IServerGameDLL*);
	KHook::Return<void> Hook_GameFrame(IServerGameDLL*, bool simulating, bool bFirstTick, bool bLastTick);
	KHook::Return<void> Hook_DispatchConCommand(ICvar*, ConCommandRef cmd, const CCommandContext &ctx, const CCommand &args);
public:
	const char *GetAuthor() { return PLUGIN_AUTHOR; }
	const char *GetName() { return "Workshop Host Map"; }
	const char *GetDescription() { return "Lets players host a workshop map from chat with !hostmap <id>."; }
	const char *GetURL() { return PLUGIN_URL; }
	const char *GetLicense() { return PLUGIN_LICENSE; }
	const char *GetVersion() { return PLUGIN_FULL_VERSION; }
	const char *GetDate() { return __DATE__; }
	const char *GetLogTag() { return "WSHostMap"; }
protected:
	KHook::Virtual<IServerGameDLL, void> m_GameServerSteamAPIActivated;
	KHook::Virtual<IServerGameDLL, void, bool, bool, bool> m_GameFrame;
	KHook::Virtual<ICvar, void, ConCommandRef, const CCommandContext &, const CCommand &> m_DispatchConCommand;
};

extern WSHostMapPlugin g_ThisPlugin;

PLUGIN_GLOBALVARS();

#endif // _INCLUDE_WSHOSTMAP_PLUGIN_H_
