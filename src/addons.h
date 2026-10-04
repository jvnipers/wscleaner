#ifndef _INCLUDE_WSCLEANER_ADDONS_H_
#define _INCLUDE_WSCLEANER_ADDONS_H_

// Delete downloaded workshop addons the server is not using, then bring Steam's and the workshop manager's
// view of installed items back in line with the disk.
// `currentMap` is the running level's name; the workshop addon that provides it is always kept.
void CleanupWorkshopAddons(const char *currentMap);

#endif // _INCLUDE_WSCLEANER_ADDONS_H_
