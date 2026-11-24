#include "HUMAClothVelocityVertexFactory.h"
#include "MeshDrawShaderBindings.h"
#include "MeshMaterialShader.h"
#include "ShaderParameterUtils.h"

class FHUMAClothVelocityShaderParameters : public FLocalVertexFactoryShaderParametersBase
{
    DECLARE_TYPE_LAYOUT(FHUMAClothVelocityShaderParameters, NonVirtual);
public:
    void GetElementShaderBindings(const FSceneInterface* Scene, const FSceneView* View, const FMeshMaterialShader* Shader,
        EVertexInputStreamType InputStreamType, ERHIFeatureLevel::Type FeatureLevel, const FVertexFactory* VertexFactory,
        const FMeshBatchElement& BatchElement, FMeshDrawSingleShaderBindings& Bindings, FVertexInputStreamArray& Streams) const
    {
        const auto* Factory = static_cast<const FHUMAClothVelocityVertexFactory*>(VertexFactory);
        GetElementShaderBindingsBase(Scene, View, Shader, InputStreamType, FeatureLevel, VertexFactory, BatchElement, Factory->GetUniformBuffer(), Bindings, Streams);
        Bindings.Add(Shader->GetUniformBufferParameter<FGPUSkinPassThroughFactoryLooseParameters>(), Factory->VelocityUniform);
    }
};

IMPLEMENT_TYPE_LAYOUT(FHUMAClothVelocityShaderParameters);
IMPLEMENT_VERTEX_FACTORY_PARAMETER_TYPE(FHUMAClothVelocityVertexFactory, SF_Vertex, FHUMAClothVelocityShaderParameters);
IMPLEMENT_VERTEX_FACTORY_PARAMETER_TYPE(FHUMAClothVelocityVertexFactory, SF_Pixel, FHUMAClothVelocityShaderParameters);
IMPLEMENT_VERTEX_FACTORY_TYPE(FHUMAClothVelocityVertexFactory, "/Engine/Private/LocalVertexFactory.ush",
    EVertexFactoryFlags::UsedWithMaterials | EVertexFactoryFlags::SupportsDynamicLighting |
    EVertexFactoryFlags::SupportsPrecisePrevWorldPos | EVertexFactoryFlags::SupportsPrimitiveIdStream |
    EVertexFactoryFlags::SupportsManualVertexFetch | EVertexFactoryFlags::SupportsGPUSkinPassThrough);

void FHUMAClothVelocityVertexFactory::UpdateVelocity(FRHICommandListBase& Commands, uint32 FrameNumber,
    FRHIShaderResourceView* Current, FRHIShaderResourceView* Previous, FRHIShaderResourceView* RestTangents)
{
    FGPUSkinPassThroughFactoryLooseParameters Parameters;
    Parameters.FrameNumber = FrameNumber;
    Parameters.PositionBuffer = Current;
    Parameters.PreviousPositionBuffer = Previous;
    Parameters.PreSkinnedTangentBuffer = RestTangents;
    if (VelocityUniform) VelocityUniform.UpdateUniformBufferImmediate(Commands, Parameters);
    else VelocityUniform = TUniformBufferRef<FGPUSkinPassThroughFactoryLooseParameters>::CreateUniformBufferImmediate(Parameters, UniformBuffer_MultiFrame);
}