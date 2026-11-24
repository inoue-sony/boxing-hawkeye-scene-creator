#include "HUMAClothSourceTransfer.h"

#include "GlobalShader.h"
#include "ProfilingDebugging/CsvProfiler.h"
#include "RenderGraphUtils.h"
#include "ShaderParameterStruct.h"

CSV_DEFINE_CATEGORY(HUMAClothSourceLOD, true);

struct FHUMAClothSourceTransferBuffers
{
    TRefCountPtr<FRDGPooledBuffer> Triangles, Barycentrics;
};

class FHUMAClothSourceTransferCS : public FGlobalShader
{
    DECLARE_GLOBAL_SHADER(FHUMAClothSourceTransferCS);
    SHADER_USE_PARAMETER_STRUCT(FHUMAClothSourceTransferCS, FGlobalShader);
    BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
        SHADER_PARAMETER(uint32, Count)
        SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float>, SourcePositions)
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<int3>, SourceTriangles)
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float3>, Barycentrics)
        SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float>, BodyPositions)
    END_SHADER_PARAMETER_STRUCT()
};

IMPLEMENT_GLOBAL_SHADER(FHUMAClothSourceTransferCS, "/Plugin/HUMAClothBind/Private/HUMAClothSourceTransfer.usf", "GatherSourceLODCS", SF_Compute);

FRDGBufferRef ResolveHUMAClothSourceLOD(FRDGBuilder& Graph, FRDGBufferRef Positions,
    const FHUMAClothLODTransfer* Transfer, FPrimitiveComponentId Source, ERHIFeatureLevel::Type FeatureLevel,
    FHUMAClothTransferInputs& Inputs)
{
    if (!Positions || !Transfer) return Positions;
    const FHUMAClothTransferKey Key{Source, Transfer, Positions};
    if (const auto* Existing = Inputs.Find(Key)) return *Existing;
    if (Positions->Desc.GetSize() < uint64(Transfer->SourceVertexCount) * sizeof(FVector3f))
    {
        UE_LOG(LogTemp, Error, TEXT("HUMA source LOD transfer received a different LOD's vertex buffer."));
        return nullptr;
    }
    if (!Transfer->GPU) Transfer->GPU = MakeShared<FHUMAClothSourceTransferBuffers, ESPMode::ThreadSafe>();
    auto& GPU = *Transfer->GPU;
    if (!GPU.Triangles)
    {
        auto* Buffer = CreateStructuredBuffer(Graph, TEXT("HUMACloth.SourceTriangles"), sizeof(FIntVector),
            Transfer->Triangles.Num(), Transfer->Triangles.GetData(), Transfer->Triangles.Num() * sizeof(FIntVector));
        GPU.Triangles = Graph.ConvertToExternalBuffer(Buffer);
    }
    if (!GPU.Barycentrics)
    {
        auto* Buffer = CreateStructuredBuffer(Graph, TEXT("HUMACloth.SourceBarycentrics"), sizeof(FVector3f),
            Transfer->Barycentrics.Num(), Transfer->Barycentrics.GetData(), Transfer->Barycentrics.Num() * sizeof(FVector3f));
        GPU.Barycentrics = Graph.ConvertToExternalBuffer(Buffer);
    }
    auto* Output = Graph.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(float), Transfer->Triangles.Num() * 3),
        TEXT("HUMACloth.TransferredBody"));
    auto* Parameters = Graph.AllocParameters<FHUMAClothSourceTransferCS::FParameters>();
    Parameters->Count = Transfer->Triangles.Num();
    Parameters->SourcePositions = Graph.CreateSRV(Positions, PF_R32_FLOAT);
    Parameters->SourceTriangles = Graph.CreateSRV(Graph.RegisterExternalBuffer(GPU.Triangles));
    Parameters->Barycentrics = Graph.CreateSRV(Graph.RegisterExternalBuffer(GPU.Barycentrics));
    Parameters->BodyPositions = Graph.CreateUAV(Output, PF_R32_FLOAT);
    TShaderMapRef<FHUMAClothSourceTransferCS> Shader(GetGlobalShaderMap(FeatureLevel));
    FComputeShaderUtils::AddPass(Graph, RDG_EVENT_NAME("HUMACloth SourceLODTransfer %u", Source.PrimIDValue),
        Shader, Parameters, FComputeShaderUtils::GetGroupCount(Parameters->Count, 256));
    CSV_CUSTOM_STAT(HUMAClothSourceLOD, Dispatches, 1, ECsvCustomStatOp::Accumulate);
    Inputs.Add(Key, Output);
    return Output;
}
