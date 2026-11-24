#pragma once

#include "CoreMinimal.h"
#include "GPUSkinVertexFactory.h"
#include "LocalVertexFactory.h"

class FHUMAClothVelocityVertexFactory : public FLocalVertexFactory
{
    DECLARE_VERTEX_FACTORY_TYPE(FHUMAClothVelocityVertexFactory);
public:
    explicit FHUMAClothVelocityVertexFactory(ERHIFeatureLevel::Type FeatureLevel)
        : FLocalVertexFactory(FeatureLevel, "HUMAClothVelocity") { bGPUSkinPassThrough = true; }
    virtual bool SupportsPositionOnlyStream() const override { return false; }
    virtual bool SupportsPositionAndNormalOnlyStream() const override { return false; }
    TUniformBufferRef<FGPUSkinPassThroughFactoryLooseParameters> VelocityUniform;
    void UpdateVelocity(FRHICommandListBase& Commands, uint32 FrameNumber, FRHIShaderResourceView* Current, FRHIShaderResourceView* Previous, FRHIShaderResourceView* RestTangents);
    virtual void ReleaseRHI() override { VelocityUniform.SafeRelease(); FLocalVertexFactory::ReleaseRHI(); }
};