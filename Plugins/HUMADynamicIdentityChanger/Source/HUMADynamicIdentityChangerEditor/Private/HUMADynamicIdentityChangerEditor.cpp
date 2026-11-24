#include "Modules/ModuleManager.h"
#include "HAL/PlatformProcess.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"
#include "HUMAIdentity.h"

namespace
{
	void* Hdf5Handle = nullptr;
}

bool IsHUMAIdentityH5Available()
{
	return Hdf5Handle != nullptr;
}

class FHUMAIdentityEditorModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("HUMADynamicIdentityChanger"));
		if (Plugin)
		{
			const FString Filename = FPaths::Combine(Plugin->GetBaseDir(), TEXT("ThirdParty/HDF5/bin/hdf5.dll"));
			Hdf5Handle = FPlatformProcess::GetDllHandle(*Filename);
		}
		if (!Hdf5Handle)
		{
			UE_LOG(LogHUMAIdentity, Error, TEXT("HDF5 library could not be loaded. Restore this plugin's ThirdParty/HDF5 files before importing H5."));
		}
	}

	virtual void ShutdownModule() override
	{
		if (Hdf5Handle)
		{
			FPlatformProcess::FreeDllHandle(Hdf5Handle);
			Hdf5Handle = nullptr;
		}
	}
};

IMPLEMENT_MODULE(FHUMAIdentityEditorModule, HUMADynamicIdentityChangerEditor)
