#include "rtti.h"

#include <algorithm>
#include <cstring>
#include <string>

#ifdef _WIN32
#include <windows.h>
#else
#include <link.h>
#endif

bool GetModuleFromAddress(void *address, ModuleInfo &outModule)
{
	outModule.base = nullptr;
	outModule.ranges.clear();
#ifdef _WIN32
	HMODULE hModule = nullptr;
	if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)address, &hModule))
		return false;
	uint8_t *base = (uint8_t *)hModule;
	IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
	IMAGE_SECTION_HEADER *section = IMAGE_FIRST_SECTION(nt);
	outModule.base = base;
	for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i)
	{
		ModuleRange range;
		range.start = base + section[i].VirtualAddress;
		range.size = section[i].Misc.VirtualSize;
		range.writable = (section[i].Characteristics & IMAGE_SCN_MEM_WRITE) != 0;
		range.executable = (section[i].Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0;
		outModule.ranges.push_back(range);
	}
#else
	struct Context_t
	{
		uintptr_t address;
		ModuleInfo *module;
	} context = { (uintptr_t)address, &outModule };
	dl_iterate_phdr([](struct dl_phdr_info *info, size_t, void *data) -> int
	{
		Context_t *ctx = (Context_t *)data;
		bool contains = false;
		for (int i = 0; i < info->dlpi_phnum; ++i)
		{
			const ElfW(Phdr) &phdr = info->dlpi_phdr[i];
			uintptr_t start = info->dlpi_addr + phdr.p_vaddr;
			if (phdr.p_type == PT_LOAD && ctx->address >= start && ctx->address < start + phdr.p_memsz)
				contains = true;
		}
		if (!contains)
			return 0;
		ctx->module->base = (uint8_t *)info->dlpi_addr;
		for (int i = 0; i < info->dlpi_phnum; ++i)
		{
			const ElfW(Phdr) &phdr = info->dlpi_phdr[i];
			if (phdr.p_type != PT_LOAD)
				continue;
			ModuleRange range;
			range.start = (uint8_t *)(info->dlpi_addr + phdr.p_vaddr);
			range.size = phdr.p_memsz;
			range.writable = (phdr.p_flags & PF_W) != 0;
			range.executable = (phdr.p_flags & PF_X) != 0;
			ctx->module->ranges.push_back(range);
		}
		return 1;
	}, &context);
#endif
	return outModule.base != nullptr && !outModule.ranges.empty();
}

// Every address in the module's data where the NUL-terminated `text` starts. A match always has a readable byte before it.
static std::vector<uint8_t *> FindStrings(const ModuleInfo &module, const std::string &text)
{
	std::vector<uint8_t *> matches;
	const uint8_t *needle = (const uint8_t *)text.c_str();
	size_t needleSize = text.size() + 1;
	for (const ModuleRange &range : module.ranges)
	{
		if (range.executable)
			continue;
		uint8_t *end = range.start + range.size;
		for (uint8_t *it = range.start + 1; (it = std::search(it, end, needle, needle + needleSize)) != end; ++it)
			matches.push_back(it);
	}
	return matches;
}

// Every pointer-aligned slot in the module's data that holds `value`.
static std::vector<uint8_t *> FindPointers(const ModuleInfo &module, uintptr_t value, bool writableOnly)
{
	std::vector<uint8_t *> matches;
	for (const ModuleRange &range : module.ranges)
	{
		if (range.executable || (writableOnly && !range.writable))
			continue;
		uintptr_t start = ((uintptr_t)range.start + sizeof(uintptr_t) - 1) & ~(uintptr_t)(sizeof(uintptr_t) - 1);
		uintptr_t end = (uintptr_t)range.start + range.size;
		for (uintptr_t slot = start; slot + sizeof(uintptr_t) <= end; slot += sizeof(uintptr_t))
		{
			if (*(uintptr_t *)slot == value)
				matches.push_back((uint8_t *)slot);
		}
	}
	return matches;
}

#ifdef _WIN32
struct CompleteObjectLocator_t
{
	uint32_t signature; // 1 on x64, where the other fields are image relative
	uint32_t offset;    // offset of this vtable's subobject inside the complete object
	uint32_t cdOffset;
	uint32_t typeDescriptor;
	uint32_t classDescriptor;
	uint32_t self;
};

void *FindVTable(const ModuleInfo &module, const char *className)
{
	std::vector<void *> vtables;
	// TypeDescriptor: vtable pointer, spare pointer, then the decorated name.
	for (uint8_t *name : FindStrings(module, std::string(".?AV") + className + "@@"))
	{
		uint32_t typeDescriptor = (uint32_t)(name - 2 * sizeof(void *) - module.base);
		for (const ModuleRange &range : module.ranges)
		{
			if (range.executable || range.writable)
				continue;
			for (uint8_t *it = range.start; it + sizeof(CompleteObjectLocator_t) <= range.start + range.size; it += sizeof(uint32_t))
			{
				const CompleteObjectLocator_t *col = (const CompleteObjectLocator_t *)it;
				if (col->signature != 1 || col->offset != 0 || col->typeDescriptor != typeDescriptor || col->self != (uint32_t)(it - module.base))
					continue;
				// The locator sits in the slot right before the vtable's first function.
				for (uint8_t *slot : FindPointers(module, (uintptr_t)col, false))
					vtables.push_back(slot + sizeof(void *));
			}
		}
	}
	return vtables.size() == 1 ? vtables[0] : nullptr;
}
#else
void *FindVTable(const ModuleInfo &module, const char *className)
{
	std::vector<void *> vtables;
	// Itanium typeinfo names are the length-prefixed class name.
	std::string mangled = std::to_string(strlen(className)) + className;
	for (uint8_t *name : FindStrings(module, mangled))
	{
		// Skip matches inside longer names, such as template arguments.
		if (name[-1] != '\0')
			continue;
		// typeinfo: vtable pointer of the typeinfo class, then the name pointer.
		for (uint8_t *nameSlot : FindPointers(module, (uintptr_t)name, false))
		{
			uint8_t *typeinfo = nameSlot - sizeof(void *);
			// vtable group: offset to top, typeinfo pointer, then the functions. Offset 0 is the primary vtable.
			for (uint8_t *typeinfoSlot : FindPointers(module, (uintptr_t)typeinfo, false))
			{
				if (*(intptr_t *)(typeinfoSlot - sizeof(void *)) == 0)
					vtables.push_back(typeinfoSlot + sizeof(void *));
			}
		}
	}
	return vtables.size() == 1 ? vtables[0] : nullptr;
}
#endif

void *FindObjectByVTable(const ModuleInfo &module, void *vtable)
{
	std::vector<uint8_t *> objects = FindPointers(module, (uintptr_t)vtable, true);
	return objects.size() == 1 ? objects[0] : nullptr;
}
