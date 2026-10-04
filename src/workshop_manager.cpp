#include "plugin.h"
#include "workshop_manager.h"
#include "rtti.h"
#include "tier1/utlmap.h"
#include "tier1/utlstring.h"
#include "tier1/bufferstring.h"

// Layout reversed from server.dll and libserver.so. The class is a CBaseGameSystem with an IUGCAddonPathResolver
// at +0x10; only the members this plugin touches are spelled out.

struct WorkshopMapInfo_t
{
	CBufferStringN<200> m_szFilePath;
	CUtlString m_szMapName;
};

struct RequestedMap_t
{
	int m_nState; // 0 query, 1 waiting on query, 2 check item state, 3 downloading, 4 failed, 5 installed
	SteamUGCDetails_t m_details;
	UGCQueryHandle_t m_hQuery;
};

// The engine instantiates these with CDefLess<uint64>, so lookups here run the same inlined comparison.
typedef CUtlOrderedMap<uint64, RequestedMap_t *> WorkshopRequestMap_t;
typedef CUtlOrderedMap<uint64, WorkshopMapInfo_t *> WorkshopLoadedMap_t;
typedef CUtlOrderedMap<uint64, bool> WorkshopStatusMap_t;

class CDedicatedServerWorkshopManager
{
public:
	void *m_pVTable;                             // 0x00
	const char *m_pszGameSystemName;             // 0x08
	uint8 m_Pad[0x60];                           // 0x10, path resolver vtable and the two Steam callbacks
	WorkshopRequestMap_t m_requestedMaps;        // 0x70, downloads in flight
	WorkshopLoadedMap_t m_mapLoadedWorkshopMaps; // 0x98, maps the server can load, backs ds_workshop_listmaps
	WorkshopStatusMap_t m_mapStatus;             // 0xC0, members of the collection being hosted, all must stay loaded
	uint64 m_nRequestedSharedFileId;             // 0xE8
	uint64 m_nRequestedCollectionSharedFileId;   // 0xF0
	bool m_bInitialized;                         // 0xF8
};

static_assert(sizeof(WorkshopRequestMap_t) == 0x28, "CUtlOrderedMap layout drifted from the engine's");
static_assert(sizeof(WorkshopMapInfo_t) == 216, "WorkshopMapInfo_t layout drifted from the engine's");
#ifdef _WIN32
// server.dll allocates 9800 bytes, reads the query handle at +9792 and compares the int at +8180 with
// GetItemInstallInfo's timestamp. Steam packs its structs to 4 bytes on Linux, so these only hold on Windows.
// Only the keys of m_requestedMaps are read here, never the entries.
static_assert(sizeof(RequestedMap_t) == 9800, "RequestedMap_t layout drifted from the engine's");
static_assert(offsetof(RequestedMap_t, m_hQuery) == 9792, "RequestedMap_t layout drifted from the engine's");
static_assert(offsetof(RequestedMap_t, m_details.m_rtimeUpdated) == 8180, "RequestedMap_t layout drifted from the engine's");
#endif
static_assert(offsetof(CDedicatedServerWorkshopManager, m_mapLoadedWorkshopMaps) == 0x98, "workshop manager layout drifted");
static_assert(offsetof(CDedicatedServerWorkshopManager, m_nRequestedSharedFileId) == 0xE8, "workshop manager layout drifted");
static_assert(sizeof(CDedicatedServerWorkshopManager) == 0x100, "workshop manager layout drifted");

static CDedicatedServerWorkshopManager *GetWorkshopManager()
{
	static bool s_bSearched = false;
	static CDedicatedServerWorkshopManager *s_pManager = nullptr;
	if (s_bSearched)
		return s_pManager;
	s_bSearched = true;

	// The manager is a single static object in the server module, found through its class's RTTI.
	ModuleInfo server;
	void *anchor = (*(void ***)g_pSource2Server)[0];
	void *vtable = GetModuleFromAddress(anchor, server) ? FindVTable(server, "CDedicatedServerWorkshopManager") : nullptr;
	CDedicatedServerWorkshopManager *pManager = vtable ? (CDedicatedServerWorkshopManager *)FindObjectByVTable(server, vtable) : nullptr;
	if (!pManager || !pManager->m_pszGameSystemName || V_strcmp(pManager->m_pszGameSystemName, "DedicatedServerWorkshopManager") != 0)
	{
		META_CONPRINTF("[WSCleaner] Could not find the workshop manager, workshop manager sync disabled.\n");
		return nullptr;
	}
	s_pManager = pManager;
	return s_pManager;
}

void GetWorkshopManagerBusyAddons(std::set<uint64> &outList)
{
	CDedicatedServerWorkshopManager *pManager = GetWorkshopManager();
	if (!pManager)
		return;
	// Directories of downloads in flight can be half written.
	const WorkshopRequestMap_t &requested = pManager->m_requestedMaps;
	for (int i = 0; i < requested.MaxElement(); ++i)
	{
		if (requested.IsValidIndex(i))
			outList.insert(requested.Key(i));
	}
	// The engine asserts fatally if a collection member disappears from the loaded map before the
	// collection's mapgroup is built.
	const WorkshopStatusMap_t &status = pManager->m_mapStatus;
	for (int i = 0; i < status.MaxElement(); ++i)
	{
		if (status.IsValidIndex(i))
			outList.insert(status.Key(i));
	}
	if (pManager->m_nRequestedSharedFileId != 0)
		outList.insert(pManager->m_nRequestedSharedFileId);
}

void ForgetWorkshopManagerMap(uint64 addonID)
{
	CDedicatedServerWorkshopManager *pManager = GetWorkshopManager();
	if (!pManager)
		return;
	// The loaded map backs ds_workshop_listmaps, ds_workshop_changelevel, changelevel by workshop map name and the
	// addon id to vpk path lookup, so a stale entry would point at a deleted file.
	WorkshopLoadedMap_t &loaded = pManager->m_mapLoadedWorkshopMaps;
	int index = loaded.Find(addonID);
	if (index == loaded.InvalidIndex())
		return;
	WorkshopMapInfo_t *pInfo = loaded.Element(index);
	loaded.RemoveAt(index);
	delete pInfo;
	META_CONPRINTF("[WSCleaner] Removed addon from workshop manager: %llu\n", addonID);
}
