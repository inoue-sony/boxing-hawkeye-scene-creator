#include "Modules/ModuleManager.h"
#include "HUMAClothReadback.h"
#include "HUMAClothGpuState.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"
#include "ShaderCore.h"

class FHUMAClothBindModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		const FString ShaderDirectory = FPaths::Combine(IPluginManager::Get().FindPlugin(TEXT("HUMAClothBind"))->GetBaseDir(), TEXT("Shaders"));
		AddShaderSourceDirectoryMapping(TEXT("/Plugin/HUMAClothBind"), ShaderDirectory);
		StartHUMAClothRenderer();
		StartHUMAClothGPU();
	}
	virtual void ShutdownModule() override { StopHUMAClothGPU(); StopHUMAClothRenderer(); }
};

IMPLEMENT_MODULE(FHUMAClothBindModule, HUMAClothBind)