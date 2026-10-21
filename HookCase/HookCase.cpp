#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOLib.h>
#include <kern/clock.h>
#include <kern/energy_perf.h>
#include <libkern/libkern.h>
#include <mach/kmod.h>
#include <mach/vm_param.h>

#include <stdint.h>

#define HOOKCASE_EXPORT __attribute__((visibility("default")))

template <typename T>
static inline T &getMember(void *object, size_t offset) {
	return *reinterpret_cast<T *>(static_cast<uint8_t *>(object) + offset);
}

class AppleIntelBaseController {
public:
	HOOKCASE_EXPORT uint32_t ReadRegister32(unsigned long regOffset);
	HOOKCASE_EXPORT void WriteRegister32(unsigned long regOffset, uint32_t value);
	HOOKCASE_EXPORT uint64_t ReadRegister64(volatile void *rmmio, unsigned long regOffset);
	HOOKCASE_EXPORT void WriteRegister64(volatile void *rmmio, unsigned long regOffset, uint64_t value);
	HOOKCASE_EXPORT uint64_t getPMTNow();
	HOOKCASE_EXPORT IOReturn hwSetupDSBMemory();
};

class AppleIntelPortHAL {
public:
	HOOKCASE_EXPORT uint32_t probePortMode();
};

class AppleIntelDisplayPath {
public:
	HOOKCASE_EXPORT static uint32_t DSBEngineBusyStatus[3];
};

namespace {
constexpr const char *Version = "1.0.1";
constexpr size_t ControllerRmmioOffset = 0x9B0;
constexpr size_t ControllerRmmioLengthOffset = 0xC78;
constexpr size_t RegisterManagerRmmioOffset = 0x50;
constexpr size_t RegisterManagerRmmioLengthOffset = 0x60;

static volatile uint32_t invalidRead32Logs;
static volatile uint32_t invalidWrite32Logs;
static volatile uint32_t invalidRead64Logs;
static volatile uint32_t invalidWrite64Logs;
static volatile uint32_t dsbSetupLogs;
static volatile uint32_t portModeLogs;
static volatile uint32_t gpuDescribeLogs;
static volatile uint32_t gpuAccumulateLogs;

static inline bool shouldLog(volatile uint32_t &counter, uint32_t limit) {
	return __sync_fetch_and_add(&counter, 1U) < limit;
}

static inline volatile uint8_t *controllerRmmio(AppleIntelBaseController *controller) {
	return reinterpret_cast<volatile uint8_t *>(
		getMember<uintptr_t>(controller, ControllerRmmioOffset));
}

static inline uint32_t controllerRmmioLength(AppleIntelBaseController *controller) {
	return getMember<uint32_t>(controller, ControllerRmmioLengthOffset);
}

static bool resolveExplicitRmmio(AppleIntelBaseController *controller, volatile void *rmmio,
	unsigned long regOffset, size_t width, volatile uint8_t *&resolved) {
	auto *base = controllerRmmio(controller);
	const uint32_t length = controllerRmmioLength(controller);
	if (!base || !rmmio || width == 0 || length < width) {
		return false;
	}

	const uintptr_t baseAddress = reinterpret_cast<uintptr_t>(base);
	const uintptr_t rmmioAddress = reinterpret_cast<uintptr_t>(rmmio);
	if (rmmioAddress < baseAddress) {
		return false;
	}

	const uint64_t delta = static_cast<uint64_t>(rmmioAddress - baseAddress);
	if (delta > length) {
		return false;
	}

	const uint64_t remaining = static_cast<uint64_t>(length) - delta;
	if (remaining < width || static_cast<uint64_t>(regOffset) > remaining - width ||
		static_cast<uint64_t>(regOffset) > UINTPTR_MAX - rmmioAddress) {
		return false;
	}

	resolved = reinterpret_cast<volatile uint8_t *>(rmmioAddress + regOffset);
	return true;
}
}

uint32_t AppleIntelBaseController::ReadRegister32(unsigned long regOffset) {
	auto *rmmio = controllerRmmio(this);
	const uint32_t length = controllerRmmioLength(this);
	if (!rmmio || length < sizeof(uint32_t) || regOffset > length - sizeof(uint32_t)) {
		if (shouldLog(invalidRead32Logs, 8)) {
			IOLog("HookCaseBridge %s: ReadRegister32 rejected base=%p len=0x%x reg=0x%lx\n",
				Version, rmmio, length, regOffset);
		}
		return 0;
	}
	return *reinterpret_cast<volatile uint32_t *>(rmmio + regOffset);
}

void AppleIntelBaseController::WriteRegister32(unsigned long regOffset, uint32_t value) {
	auto *rmmio = controllerRmmio(this);
	const uint32_t length = controllerRmmioLength(this);
	if (!rmmio || length < sizeof(uint32_t) || regOffset > length - sizeof(uint32_t)) {
		if (shouldLog(invalidWrite32Logs, 8)) {
			IOLog("HookCaseBridge %s: WriteRegister32 rejected base=%p len=0x%x reg=0x%lx\n",
				Version, rmmio, length, regOffset);
		}
		return;
	}
	*reinterpret_cast<volatile uint32_t *>(rmmio + regOffset) = value;
}

uint64_t AppleIntelBaseController::ReadRegister64(volatile void *rmmio, unsigned long regOffset) {
	volatile uint8_t *resolved = nullptr;
	if (!resolveExplicitRmmio(this, rmmio, regOffset, sizeof(uint64_t), resolved)) {
		if (shouldLog(invalidRead64Logs, 8)) {
			IOLog("HookCaseBridge %s: ReadRegister64 rejected base=%p ptr=%p len=0x%x reg=0x%lx\n",
				Version, controllerRmmio(this), rmmio, controllerRmmioLength(this), regOffset);
		}
		return 0;
	}
	return *reinterpret_cast<volatile uint64_t *>(resolved);
}

void AppleIntelBaseController::WriteRegister64(volatile void *rmmio, unsigned long regOffset,
	uint64_t value) {
	volatile uint8_t *resolved = nullptr;
	if (!resolveExplicitRmmio(this, rmmio, regOffset, sizeof(uint64_t), resolved)) {
		if (shouldLog(invalidWrite64Logs, 8)) {
			IOLog("HookCaseBridge %s: WriteRegister64 rejected base=%p ptr=%p len=0x%x reg=0x%lx\n",
				Version, controllerRmmio(this), rmmio, controllerRmmioLength(this), regOffset);
		}
		return;
	}
	*reinterpret_cast<volatile uint64_t *>(resolved) = value;
}

uint64_t AppleIntelBaseController::getPMTNow() {
	uint64_t uptime = 0;
	uint64_t nanoseconds = 0;
	clock_get_uptime(&uptime);
	absolutetime_to_nanoseconds(uptime, &nanoseconds);
	return nanoseconds;
}

IOReturn AppleIntelBaseController::hwSetupDSBMemory() {
	constexpr uint32_t PipeCount = 3;
	constexpr uint32_t DsbCount = 3;
	constexpr uint32_t MaximumDsbBytes = 16U * 1024U * 1024U;
	constexpr uint32_t DsbChickenSkipWaits = 1U << 23;
	constexpr uint32_t DsbChickenControlSafeWindow = 1U << 15;
	constexpr uint32_t DsbChickenControlNoVblank = 1U << 14;
	constexpr uint32_t DsbChickenInstructionSafeWindow = 1U << 7;
	constexpr uint32_t DsbChickenInstructionNoVblank = 1U << 6;

	auto *&buffer = getMember<IOBufferMemoryDescriptor *>(this, 0xCD8);
	if (buffer) {
		if (shouldLog(dsbSetupLogs, 16)) {
			IOLog("HookCaseBridge %s: DSB already configured descriptor=%p\n", Version, buffer);
		}
		return kIOReturnSuccess;
	}

	const uint32_t size = getMember<uint32_t>(this, 0xDCC);
	if (size == 0 || size > MaximumDsbBytes || (size % page_size) != 0) {
		if (shouldLog(dsbSetupLogs, 16)) {
			IOLog("HookCaseBridge %s: DSB rejected size=0x%x page=0x%lx\n",
				Version, size, static_cast<unsigned long>(page_size));
		}
		return kIOReturnBadArgument;
	}

	const uint64_t dsbOffset = getMember<uint64_t>(this, 0xC50);
	auto *gttMmio = getMember<volatile void *>(this, 0xCA0);
	const uint64_t lastBufferOffset = static_cast<uint64_t>(size) - page_size;
	if (dsbOffset > UINT64_MAX - lastBufferOffset) {
		if (shouldLog(dsbSetupLogs, 16)) {
			IOLog("HookCaseBridge %s: DSB rejected overflowing offset=0x%llx size=0x%x\n",
				Version, dsbOffset, size);
		}
		return kIOReturnBadArgument;
	}
	const uint64_t lastGttRegister = (dsbOffset + lastBufferOffset) >> 9;
	volatile uint8_t *lastGttEntry = nullptr;
	if (lastGttRegister > ULONG_MAX ||
		!resolveExplicitRmmio(this, gttMmio, static_cast<unsigned long>(lastGttRegister),
			sizeof(uint64_t), lastGttEntry)) {
		if (shouldLog(dsbSetupLogs, 16)) {
			IOLog("HookCaseBridge %s: DSB rejected GTT=%p offset=0x%llx lastReg=0x%llx\n",
				Version, gttMmio, dsbOffset, lastGttRegister);
		}
		return kIOReturnBadArgument;
	}

	buffer = IOBufferMemoryDescriptor::withOptions(
		kIOMemoryPhysicallyContiguous | kIOMemoryHostPhysicallyContiguous,
		size, page_size);
	if (!buffer) {
		if (shouldLog(dsbSetupLogs, 16)) {
			IOLog("HookCaseBridge %s: DSB contiguous allocation failed size=0x%x\n",
				Version, size);
		}
		return kIOReturnNoMemory;
	}

	const uint32_t memoryOptions = getMember<uint32_t>(this, 0xCF0);
	const uint64_t physicalAddress = buffer->getPhysicalSegment(0, nullptr, memoryOptions);
	if (!physicalAddress || physicalAddress > UINT64_MAX - lastBufferOffset) {
		if (shouldLog(dsbSetupLogs, 16)) {
			IOLog("HookCaseBridge %s: DSB physical segment invalid phys=0x%llx\n",
				Version, physicalAddress);
		}
		buffer->release();
		buffer = nullptr;
		return kIOReturnNoMemory;
	}

	for (uint64_t offset = 0; offset < size; offset += page_size) {
		WriteRegister64(gttMmio, (offset + dsbOffset) >> 9,
			((physicalAddress + offset) & 0x0000FFFFFFFFF000ULL) | 7ULL);
	}

	const bool vrrSupport = getMember<bool>(this, 0x1B62);
	const uint32_t chickenBits = DsbChickenSkipWaits |
		(vrrSupport ? DsbChickenControlSafeWindow | DsbChickenControlNoVblank |
			DsbChickenInstructionSafeWindow | DsbChickenInstructionNoVblank : 0);
	for (uint32_t pipe = 0; pipe < PipeCount; ++pipe) {
		for (uint32_t dsb = 0; dsb < DsbCount; ++dsb) {
			WriteRegister32(0x70B00 + pipe * 0x1000 + dsb * 0x100 + 0xF0, chickenBits);
		}
	}

	return kIOReturnSuccess;
}

uint32_t AppleIntelPortHAL::probePortMode() {
	enum PortMode : uint32_t {
		PortModeInvalid = 0,
		PortModeType1 = 1,
		PortModeType2 = 2,
		PortModeType3 = 3,
	};

	auto *portType = getMember<uint32_t *>(this, 0x548);
	if (!portType) {
		if (shouldLog(portModeLogs, 16)) {
			IOLog("HookCaseBridge %s: probePortMode missing type pointer\n", Version);
		}
		return PortModeInvalid;
	}

	const uint32_t type = *portType;
	if (type <= 1) {
		return PortModeType1;
	}
	if (type > 5) {
		if (shouldLog(portModeLogs, 16)) {
			IOLog("HookCaseBridge %s: probePortMode rejected type=%u\n", Version, type);
		}
		return PortModeInvalid;
	}

	auto *registerManager = getMember<void *>(this, 0x6A8);
	if (!registerManager) {
		if (shouldLog(portModeLogs, 16)) {
			IOLog("HookCaseBridge %s: probePortMode type=%u missing register manager\n",
				Version, type);
		}
		return PortModeInvalid;
	}

	auto *base = getMember<volatile uint8_t *>(registerManager, RegisterManagerRmmioOffset);
	const uint32_t length = getMember<uint32_t>(registerManager,
		RegisterManagerRmmioLengthOffset);
	constexpr uint64_t PortModeRegister = 0x1638A0;
	if (!base || length < sizeof(uint32_t) || PortModeRegister > length - sizeof(uint32_t)) {
		if (shouldLog(portModeLogs, 16)) {
			IOLog("HookCaseBridge %s: probePortMode type=%u rejected manager=%p base=%p len=0x%x\n",
				Version, type, registerManager, base, length);
		}
		return PortModeInvalid;
	}

	const uint32_t value = *reinterpret_cast<volatile uint32_t *>(base + PortModeRegister);
	uint32_t mode = PortModeInvalid;
	if (value != UINT32_MAX) {
		const unsigned shift = static_cast<unsigned>(type - 2) * 8;
		if (value & (1U << (shift + 6))) {
			mode = PortModeType2;
		} else if (value & (1U << (shift + 5))) {
			mode = PortModeType3;
		}
	}
	return mode;
}

uint32_t AppleIntelDisplayPath::DSBEngineBusyStatus[3] = {};

extern "C" HOOKCASE_EXPORT uint64_t gpu_accumulate_time(uint32_t scope, uint32_t gpuId,
	uint32_t gpuDomain, uint64_t gpuAccumulatedNs, uint64_t gpuTimestampNs) {
	(void)scope;
	(void)gpuId;
	(void)gpuDomain;
	(void)gpuAccumulatedNs;
	(void)gpuTimestampNs;
	return 0;
}

extern "C" HOOKCASE_EXPORT void gpu_describe(gpu_descriptor_t descriptor) {
	(void)descriptor;
}

extern "C" HOOKCASE_EXPORT char *strnstr(const char *s, const char *find, size_t slen) {
	char first;
	char current;
	size_t remainingLength;

	if ((first = *find++) != '\0') {
		remainingLength = strlen(find);
		do {
			do {
				if ((current = *s++) == '\0' || slen-- < 1) {
					return nullptr;
				}
			} while (current != first);
			if (remainingLength > slen) {
				return nullptr;
			}
		} while (strncmp(s, find, remainingLength) != 0);
		--s;
	}

	return const_cast<char *>(s);
}

extern "C" kern_return_t HookCase_kern_start(kmod_info_t *, void *) {
	IOLog("HookCaseBridge %s loaded\n", Version);
	return KERN_SUCCESS;
}

extern "C" kern_return_t HookCase_kern_stop(kmod_info_t *, void *) {
	return KERN_SUCCESS;
}
