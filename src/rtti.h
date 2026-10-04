#ifndef _INCLUDE_WSCLEANER_RTTI_H_
#define _INCLUDE_WSCLEANER_RTTI_H_

#include <cstddef>
#include <cstdint>
#include <vector>

// Locates objects inside a loaded module through the compiler's RTTI, so no byte signatures are needed.
// Windows reads MSVC type descriptors and complete object locators, Linux reads Itanium typeinfo.

struct ModuleRange
{
	uint8_t *start;
	size_t size;
	bool writable;
	bool executable;
};

struct ModuleInfo
{
	uint8_t *base;
	std::vector<ModuleRange> ranges;
};

// Mapped sections (Windows) or PT_LOAD segments (Linux) of the module that contains `address`.
bool GetModuleFromAddress(void *address, ModuleInfo &outModule);

// Primary vtable of `className`, or null unless exactly one is found.
void *FindVTable(const ModuleInfo &module, const char *className);

// The one object in the module's writable data whose vtable pointer is `vtable`, or null unless exactly one is found.
void *FindObjectByVTable(const ModuleInfo &module, void *vtable);

#endif // _INCLUDE_WSCLEANER_RTTI_H_
