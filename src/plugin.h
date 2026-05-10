#ifndef _INCLUDE_METAMOD_SOURCE_PLUGIN_H_
#define _INCLUDE_METAMOD_SOURCE_PLUGIN_H_

#include <ISmmPlugin.h>
#include <igameevents.h>
#include <sh_vector.h>
#include <set>
#include "version_gen.h"
#include "steam/isteamugc.h"


class WSCleanerPlugin : public ISmmPlugin, public IMetamodListener
{
public:
	bool Load(PluginId id, ISmmAPI *ismm, char *error, size_t maxlen, bool late);
	bool Unload(char *error, size_t maxlen);
	void AllPluginsLoaded();
	void OnLevelInit(char const *pMapName, 
						char const *pMapEntities, 
						char const *pOldLevel, 
						char const *pLandmarkName, 
						bool loadGame, 
						bool background);

	void Hook_GameServerSteamAPIActivated();

	void DoCleanup();

	// Addons that have been mounted at any point during this server session.
	// Once an addon has been seen mounted, the plugin will not auto-delete it
	// for the remainder of the session, even if it is not currently mounted.
	std::set<uint64> m_SessionLoadedAddons;
public:
	const char *GetAuthor() { return PLUGIN_AUTHOR; }
	const char *GetName() { return PLUGIN_DISPLAY_NAME; }
	const char *GetDescription() { return PLUGIN_DESCRIPTION; }
	const char *GetURL() { return PLUGIN_URL; }
	const char *GetLicense() { return PLUGIN_LICENSE; }
	const char *GetVersion() { return PLUGIN_FULL_VERSION; }
	const char *GetDate() { return __DATE__; }
	const char *GetLogTag() { return PLUGIN_LOGTAG; }
};

extern WSCleanerPlugin g_ThisPlugin;

PLUGIN_GLOBALVARS();

#endif //_INCLUDE_METAMOD_SOURCE_PLUGIN_H_
