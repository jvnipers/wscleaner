#ifndef _INCLUDE_METAMOD_SOURCE_PLUGIN_H_
#define _INCLUDE_METAMOD_SOURCE_PLUGIN_H_

#include <ISmmPlugin.h>
#include <igameevents.h>
#include "version_gen.h"
#include "steam/isteamugc.h"


class WSCleanerPlugin : public ISmmPlugin, public IMetamodListener
{
public:
	WSCleanerPlugin();

	bool Load(PluginId id, ISmmAPI *ismm, char *error, size_t maxlen, bool late);
	bool Unload(char *error, size_t maxlen);
	void AllPluginsLoaded();
	void OnLevelInit(char const *pMapName, 
						char const *pMapEntities, 
						char const *pOldLevel, 
						char const *pLandmarkName, 
						bool loadGame, 
						bool background);

	KHook::Return<void> Hook_GameServerSteamAPIActivated(ISource2Server *);
	KHook::Return<void> Hook_GameFrame(ISource2Server *, bool simulating, bool bFirstTick, bool bLastTick);
	KHook::Return<void> Hook_ServerHibernationUpdate(ISource2Server *, bool bHibernating);

	void DoCleanup();
	void RunPendingCleanup();
public:
	const char *GetAuthor() { return PLUGIN_AUTHOR; }
	const char *GetName() { return PLUGIN_DISPLAY_NAME; }
	const char *GetDescription() { return PLUGIN_DESCRIPTION; }
	const char *GetURL() { return PLUGIN_URL; }
	const char *GetLicense() { return PLUGIN_LICENSE; }
	const char *GetVersion() { return PLUGIN_FULL_VERSION; }
	const char *GetDate() { return __DATE__; }
	const char *GetLogTag() { return PLUGIN_LOGTAG; }

private:
	KHook::Virtual<ISource2Server, void> m_GameServerSteamAPIActivated;
	KHook::Virtual<ISource2Server, void, bool, bool, bool> m_GameFrame;
	KHook::Virtual<ISource2Server, void, bool> m_ServerHibernationUpdate;
	bool m_bCleanupPending = false;
};

extern WSCleanerPlugin g_ThisPlugin;

PLUGIN_GLOBALVARS();

#endif //_INCLUDE_METAMOD_SOURCE_PLUGIN_H_
