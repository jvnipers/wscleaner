#ifndef _INCLUDE_WSCLEANER_WORKSHOP_MANAGER_H_
#define _INCLUDE_WSCLEANER_WORKSHOP_MANAGER_H_

#include <set>
#include "tier0/platform.h"

// Access to server.dll's CDedicatedServerWorkshopManager, the game system behind host_workshop_map,
// host_workshop_collection, ds_workshop_listmaps, ds_workshop_changelevel and workshop changelevel.
// Every function is a no-op when the manager cannot be found.

// Addons the manager is downloading, about to host, or needs for the collection it is hosting.
void GetWorkshopManagerBusyAddons(std::set<uint64> &outList);

// Drop the manager's record of a loaded workshop map that is about to be deleted from disk.
void ForgetWorkshopManagerMap(uint64 addonID);

#endif // _INCLUDE_WSCLEANER_WORKSHOP_MANAGER_H_
