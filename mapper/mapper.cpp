#include "include/mapper.hpp"


uint64_t mapper::AllocMdlMemory(HANDLE iqvw64e_device_handle, uint64_t size, uint64_t* mdlPtr) {
	/*added by psec*/
	LARGE_INTEGER LowAddress, HighAddress;
	LowAddress.QuadPart = 0;
	HighAddress.QuadPart = 0xffff'ffff'ffff'ffffULL;

	uint64_t pages = (size / PAGE_SIZE) + 1;
	auto mdl = intel_driver::MmAllocatePagesForMdl(iqvw64e_device_handle, LowAddress, HighAddress, LowAddress, pages * (uint64_t)PAGE_SIZE);
	if (!mdl) {
		Log(L"[mapper] Can't allocate pages for mdl" << std::endl);
		return { 0 };
	}

	uint32_t byteCount = 0;
	if (!intel_driver::ReadMemory(iqvw64e_device_handle, mdl + 0x028 /*_MDL : byteCount*/, &byteCount, sizeof(uint32_t))) {
		Log(L"[mapper] Can't read the _MDL : byteCount" << std::endl);
		return { 0 };
	}

	if (byteCount < size) {
		Log(L"[mapper] Couldn't allocate enough memory, cleaning up" << std::endl);
		intel_driver::MmFreePagesFromMdl(iqvw64e_device_handle, mdl);
		intel_driver::FreePool(iqvw64e_device_handle, mdl);
		return { 0 };
	}

	auto mappingStartAddress = intel_driver::MmMapLockedPagesSpecifyCache(iqvw64e_device_handle, mdl, nt::KernelMode, nt::MmCached, NULL, FALSE, nt::NormalPagePriority);
	if (!mappingStartAddress) {
		Log(L"[mapper] Can't set mdl pages cache, cleaning up." << std::endl);
		intel_driver::MmFreePagesFromMdl(iqvw64e_device_handle, mdl);
		intel_driver::FreePool(iqvw64e_device_handle, mdl);
		return { 0 };
	}

	const auto result = intel_driver::MmProtectMdlSystemAddress(iqvw64e_device_handle, mdl, PAGE_EXECUTE_READWRITE);
	if (!result) {
		Log(L"[mapper] Can't change protection for mdl pages, cleaning up" << std::endl);
		intel_driver::MmUnmapLockedPages(iqvw64e_device_handle, mappingStartAddress, mdl);
		intel_driver::MmFreePagesFromMdl(iqvw64e_device_handle, mdl);
		intel_driver::FreePool(iqvw64e_device_handle, mdl);
		return { 0 };
	}
	Log(L"[mapper] Allocated pages for mdl" << std::endl);

	if (mdlPtr)
		*mdlPtr = mdl;

	return mappingStartAddress;
}

uint64_t mapper::AllocIndependentPages(HANDLE device_handle, uint32_t size)
{
	const auto base = intel_driver::MmAllocateIndependentPagesEx(device_handle, size);
	if (!base)
	{
		Log(L"[mapper] Error allocating independent pages" << std::endl);
		return 0;
	}

	if (!intel_driver::MmSetPageProtection(device_handle, base, size, PAGE_EXECUTE_READWRITE))
	{
		Log(L"[mapper] Failed to change page protections" << std::endl);
		intel_driver::MmFreeIndependentPages(device_handle, base, size);
		return 0;
	}

	return base;
}

uint64_t mapper::MapDriver(HANDLE iqvw64e_device_handle, BYTE* data, ULONG64 param1, ULONG64 param2, bool free, bool destroyHeader, AllocationMode mode, bool PassAllocationAddressAsFirstParam, mapCallback callback, NTSTATUS* exitCode) {

	const PIMAGE_NT_HEADERS64 nt_headers = portable_executable::GetNtHeaders(data);

	if (!nt_headers) {
		Log(L"[mapper] Invalid format of PE image" << std::endl);
		return 0;
	}

	if (nt_headers->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
		Log(L"[mapper] Image is not 64 bit" << std::endl);
		return 0;
	}

	uint32_t image_size = nt_headers->OptionalHeader.SizeOfImage;

	void* local_image_base = VirtualAlloc(nullptr, image_size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
	if (!local_image_base)
		return 0;

	DWORD TotalVirtualHeaderSize = (IMAGE_FIRST_SECTION(nt_headers))->VirtualAddress;
	image_size = image_size - (destroyHeader ? TotalVirtualHeaderSize : 0);

	uint64_t kernel_image_base = 0;
	uint64_t mdlptr = 0;
	if (mode == AllocationMode::AllocateMdl) {
		kernel_image_base = AllocMdlMemory(iqvw64e_device_handle, image_size, &mdlptr);
	}
	else if (mode == AllocationMode::AllocateIndependentPages) {
		kernel_image_base = AllocIndependentPages(iqvw64e_device_handle, image_size);
	}
	else {
		kernel_image_base = intel_driver::AllocatePool(iqvw64e_device_handle, nt::POOL_TYPE::NonPagedPool, image_size);
	}

	if (!kernel_image_base) {
		Log(L"[mapper] Failed to allocate remote image in kernel" << std::endl);

		VirtualFree(local_image_base, 0, MEM_RELEASE);
		return 0;
	}

	do {
		Log(L"[mapper] Image base has been allocated at 0x" << reinterpret_cast<void*>(kernel_image_base) << std::endl);


		memcpy(local_image_base, data, nt_headers->OptionalHeader.SizeOfHeaders);

		const PIMAGE_SECTION_HEADER current_image_section = IMAGE_FIRST_SECTION(nt_headers);

		for (auto i = 0; i < nt_headers->FileHeader.NumberOfSections; ++i) {
			if ((current_image_section[i].Characteristics & IMAGE_SCN_CNT_UNINITIALIZED_DATA) > 0)
				continue;
			auto local_section = reinterpret_cast<void*>(reinterpret_cast<uint64_t>(local_image_base) + current_image_section[i].VirtualAddress);
			memcpy(local_section, reinterpret_cast<void*>(reinterpret_cast<uint64_t>(data) + current_image_section[i].PointerToRawData), current_image_section[i].SizeOfRawData);
		}

		uint64_t realBase = kernel_image_base;
		if (destroyHeader) {
			kernel_image_base -= TotalVirtualHeaderSize;
			Log(L"[mapper] Skipped 0x" << std::hex << TotalVirtualHeaderSize << L" bytes of PE Header" << std::endl);
		}

		RelocateImageByDelta(portable_executable::GetRelocs(local_image_base), kernel_image_base - nt_headers->OptionalHeader.ImageBase);

		if (!FixSecurityCookie(local_image_base, kernel_image_base))
		{
			Log(L"[mapper] Failed to fix cookie" << std::endl);
			return 0;
		}

		if (!ResolveImports(iqvw64e_device_handle, portable_executable::GetImports(local_image_base))) {
			Log(L"[mapper] Failed to resolve imports" << std::endl);
			kernel_image_base = realBase;
			break;
		}

		if (!intel_driver::WriteMemory(iqvw64e_device_handle, realBase, (PVOID)((uintptr_t)local_image_base + (destroyHeader ? TotalVirtualHeaderSize : 0)), image_size)) {
			Log(L"[mapper] Failed to write local image to remote image" << std::endl);
			kernel_image_base = realBase;
			break;
		}

		const uint64_t address_of_entry_point = kernel_image_base + nt_headers->OptionalHeader.AddressOfEntryPoint;

		Log(L"[mapper] Calling DriverEntry 0x" << reinterpret_cast<void*>(address_of_entry_point) << std::endl);

		if (callback) {
			if (!callback(&param1, &param2, realBase, image_size, mdlptr)) {
				Log(L"[mapper] Callback returns false, failed!" << std::endl);
				kernel_image_base = realBase;
				break;
			}
		}

		NTSTATUS status = 0;
		if (!intel_driver::CallKernelFunction(iqvw64e_device_handle, &status, address_of_entry_point, (PassAllocationAddressAsFirstParam ? realBase : param1), param2)) {
			Log(L"[mapper] Failed to call driver entry" << std::endl);
			kernel_image_base = realBase;
			break;
		}

		if (exitCode)
			*exitCode = status;

		Log(L"[mapper] DriverEntry returned 0x" << std::hex << status << std::endl);

		if (free) {
			Log(L"[mapper] Freeing memory" << std::endl);
			bool free_status = false;

			if (mode == AllocationMode::AllocateMdl) {
				free_status = intel_driver::MmUnmapLockedPages(iqvw64e_device_handle, realBase, mdlptr);
				free_status = (!free_status ? false : intel_driver::MmFreePagesFromMdl(iqvw64e_device_handle, mdlptr));
				free_status = (!free_status ? false : intel_driver::FreePool(iqvw64e_device_handle, mdlptr));
			}
			else if (mode == AllocationMode::AllocateIndependentPages)
			{
				free_status = intel_driver::MmFreeIndependentPages(iqvw64e_device_handle, realBase, image_size);
			}
			else {
				free_status = intel_driver::FreePool(iqvw64e_device_handle, realBase);
			}

			if (free_status) {
				Log(L"[mapper] Memory has been released" << std::endl);
			}
			else {
				Log(L"[mapper] WARNING: Failed to free memory!" << std::endl);
			}
		}



		VirtualFree(local_image_base, 0, MEM_RELEASE);
		return realBase;

	} while (false);


	VirtualFree(local_image_base, 0, MEM_RELEASE);

	Log(L"[mapper] Freeing memory" << std::endl);
	bool free_status = false;

	if (mode == AllocationMode::AllocateMdl) {
		free_status = intel_driver::MmUnmapLockedPages(iqvw64e_device_handle, kernel_image_base, mdlptr);
		free_status = (!free_status ? false : intel_driver::MmFreePagesFromMdl(iqvw64e_device_handle, mdlptr));
		free_status = (!free_status ? false : intel_driver::FreePool(iqvw64e_device_handle, mdlptr));
	}
	else if (mode == AllocationMode::AllocateIndependentPages)
	{
		free_status = intel_driver::MmFreeIndependentPages(iqvw64e_device_handle, kernel_image_base, image_size);
	}
	else {
		free_status = intel_driver::FreePool(iqvw64e_device_handle, kernel_image_base);
	}

	if (free_status) {
		Log(L"[mapper] Memory has been released" << std::endl);
	}
	else {
		Log(L"[mapper] WARNING: Failed to free memory!" << std::endl);
	}

	return 0;
}

void mapper::RelocateImageByDelta(portable_executable::vec_relocs relocs, const uint64_t delta) {
	for (const auto& current_reloc : relocs) {
		for (auto i = 0u; i < current_reloc.count; ++i) {
			const uint16_t type = current_reloc.item[i] >> 12;
			const uint16_t offset = current_reloc.item[i] & 0xFFF;

			if (type == IMAGE_REL_BASED_DIR64)
				*reinterpret_cast<uint64_t*>(current_reloc.address + offset) += delta;
		}
	}
}

bool mapper::FixSecurityCookie(void* local_image, uint64_t kernel_image_base)
{
	auto headers = portable_executable::GetNtHeaders(local_image);
	if (!headers)
		return false;

	auto load_config_directory = headers->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_LOAD_CONFIG].VirtualAddress;
	if (!load_config_directory)
	{
		Log(L"[mapper] Load config directory wasn't found, probably StackCookie not defined, fix cookie skipped" << std::endl);
		return true;
	}

	auto load_config_struct = (PIMAGE_LOAD_CONFIG_DIRECTORY)((uintptr_t)local_image + load_config_directory);
	auto stack_cookie = load_config_struct->SecurityCookie;
	if (!stack_cookie)
	{
		Log(L"[mapper] StackCookie not defined, fix cookie skipped" << std::endl);
		return true;
	}

	stack_cookie = stack_cookie - (uintptr_t)kernel_image_base + (uintptr_t)local_image;
	if (*(uintptr_t*)(stack_cookie) != 0x2B992DDFA232) {
		Log(L"[mapper] StackCookie already fixed!? this probably wrong" << std::endl);
		return false;
	}

	Log(L"[mapper] Fixing stack cookie" << std::endl);

	auto new_cookie = 0x2B992DDFA232 ^ GetCurrentProcessId() ^ GetCurrentThreadId();
	if (new_cookie == 0x2B992DDFA232)
		new_cookie = 0x2B992DDFA233;

	*(uintptr_t*)(stack_cookie) = new_cookie;
	return true;
}

bool mapper::ResolveImports(HANDLE iqvw64e_device_handle, portable_executable::vec_imports imports) {
	for (const auto& current_import : imports) {
		ULONG64 Module = utils::GetKernelModuleAddress(current_import.module_name);
		if (!Module) {
#if !defined(DISABLE_OUTPUT)
			std::cout << "[mapper] Dependency " << current_import.module_name << " wasn't found" << std::endl;
#endif
			return false;
		}

		for (auto& current_function_data : current_import.function_datas) {
			uint64_t function_address = intel_driver::GetKernelModuleExport(iqvw64e_device_handle, Module, current_function_data.name);

			if (!function_address) {
				if (Module != intel_driver::ntoskrnlAddr) {
					function_address = intel_driver::GetKernelModuleExport(iqvw64e_device_handle, intel_driver::ntoskrnlAddr, current_function_data.name);
					if (!function_address) {
#if !defined(DISABLE_OUTPUT)
						std::cout << "[mapper] Failed to resolve import " << current_function_data.name << " (" << current_import.module_name << ")" << std::endl;
#endif
						return false;
					}
				}
			}

			*current_function_data.address = function_address;
		}
	}

	return true;
}