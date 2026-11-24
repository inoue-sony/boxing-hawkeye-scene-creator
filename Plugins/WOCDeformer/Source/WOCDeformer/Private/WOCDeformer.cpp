#include "Modules/ModuleManager.h"

class FWOCDeformerModule : public IModuleInterface
{
public:
	virtual void StartupModule() override {}
	virtual void ShutdownModule() override {}
};

IMPLEMENT_MODULE(FWOCDeformerModule, WOCDeformer)
