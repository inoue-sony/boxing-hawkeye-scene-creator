#include "HUMAClothReadback.h"
#include "GlobalShader.h"
#include "Modules/ModuleManager.h"
#include "Runtime/Launch/Resources/Version.h"
#include "PrimitiveSceneInfo.h"
#include "PrimitiveSceneProxy.h"
#include "RendererInterface.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "SceneInterface.h"
#include "SkeletalMeshDeformerHelpers.h"
#include "SkeletalMeshSceneProxy.h"
#include "SkeletalRenderPublic.h"
#if ENGINE_MINOR_VERSION >= 7
#include "SkinningSceneExtensionProxy.h"
#endif
#include "ShaderParameterStruct.h"

class FHUMAClothCopyPositionsCS : public FGlobalShader
{
    DECLARE_GLOBAL_SHADER(FHUMAClothCopyPositionsCS);
    SHADER_USE_PARAMETER_STRUCT(FHUMAClothCopyPositionsCS, FGlobalShader);
    BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
        SHADER_PARAMETER(uint32, VertexCount)
        SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float>, DeformedPositions)
        SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float3>, OutPositions)
    END_SHADER_PARAMETER_STRUCT()
};

IMPLEMENT_GLOBAL_SHADER(FHUMAClothCopyPositionsCS, "/Plugin/HUMAClothBind/Private/HUMAClothReadback.usf", "CopyPositionsCS", SF_Compute);

namespace
{
TArray<TSharedPtr<FHUMAClothReadback, ESPMode::ThreadSafe>> States;
FDelegateHandle RenderHandle;

void Capture(FPostOpaqueRenderParameters& Parameters)
{
    FRDGBuilder& GraphBuilder = *Parameters.GraphBuilder;
    for (const auto& State : States)
    {
        if (!State->Readback) State->Readback = MakeUnique<FRHIGPUBufferReadback>(TEXT("HUMAClothBodyReadback"));
        if (State->bPending)
        {
            if (!State->Readback->IsReady()) continue;
            const uint32 Bytes = State->RenderVertexCount * sizeof(FVector3f);
            const void* Data = State->Readback->Lock(Bytes);
            {
                FScopeLock Lock(&State->Mutex);
                State->Positions.SetNumUninitialized(State->RenderVertexCount);
                FMemory::Memcpy(State->Positions.GetData(), Data, Bytes);
                ++State->Revision;
            }
            State->Readback->Unlock();
            State->bPending = false;
        }
        FSkeletalMeshObject* MeshObject = nullptr;
        if (const FPrimitiveSceneInfo* Info = State->Scene->GetPrimitiveSceneInfo(State->PrimitiveId))
        {
            if (const FPrimitiveSceneProxy* Proxy = Info->Proxy)
            {
#if ENGINE_MINOR_VERSION >= 7
                if (const FSkinningSceneExtensionProxy* Skinning = Proxy->GetSkinningSceneExtensionProxy())
                    MeshObject = const_cast<FSkeletalMeshObject*>(Skinning->GetMeshObject());
                else
#endif
                if (Proxy->IsSkinnedMesh() && !Proxy->IsNaniteMesh())
                    MeshObject = const_cast<FSkeletalMeshObject*>(static_cast<const FSkeletalMeshSceneProxy*>(Proxy)->GetMeshObject());
            }
        }
        FRDGBufferRef Buffer = MeshObject && MeshObject->GetLOD() == 0
            ? FSkeletalMeshDeformerHelpers::GetAllocatedPositionBuffer(GraphBuilder, MeshObject, 0) : nullptr;
        if (!Buffer || Buffer->Desc.GetSize() < State->RenderVertexCount * sizeof(FVector3f))
        {
            FScopeLock Lock(&State->Mutex);
            State->Status = TEXT("No final Mesh Deformer buffer. Enable a deformer or the standard skinning adapter and force body LOD0.");
            continue;
        }
        {
            FScopeLock Lock(&State->Mutex);
            State->Status = TEXT("Final GPU positions / async CPU preview");
        }
        FRDGBufferRef Snapshot = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(FVector3f), State->RenderVertexCount), TEXT("HUMACloth.PositionSnapshot"));
        auto* CopyParameters = GraphBuilder.AllocParameters<FHUMAClothCopyPositionsCS::FParameters>();
        CopyParameters->VertexCount = State->RenderVertexCount;
        CopyParameters->DeformedPositions = GraphBuilder.CreateSRV(Buffer, PF_R32_FLOAT);
        CopyParameters->OutPositions = GraphBuilder.CreateUAV(Snapshot);
        TShaderMapRef<FHUMAClothCopyPositionsCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
        FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("HUMACloth Final Position Snapshot"), Shader, CopyParameters, FComputeShaderUtils::GetGroupCount(State->RenderVertexCount, 256));
        AddEnqueueCopyPass(GraphBuilder, State->Readback.Get(), Snapshot, State->RenderVertexCount * sizeof(FVector3f));
        State->bPending = true;
    }
}
}

void RegisterHUMAClothReadback(TSharedPtr<FHUMAClothReadback, ESPMode::ThreadSafe> State)
{
    ENQUEUE_RENDER_COMMAND(HUMAClothRegister)([State](FRHICommandListImmediate&) { States.AddUnique(State); });
}

void UnregisterHUMAClothReadback(TSharedPtr<FHUMAClothReadback, ESPMode::ThreadSafe> State)
{
    ENQUEUE_RENDER_COMMAND(HUMAClothUnregister)([State](FRHICommandListImmediate&) { States.Remove(State); });
}

void StartHUMAClothRenderer()
{
    RenderHandle = FModuleManager::LoadModuleChecked<IRendererModule>(TEXT("Renderer")).RegisterPostOpaqueRenderDelegate(FPostOpaqueRenderDelegate::CreateStatic(&Capture));
}

void StopHUMAClothRenderer()
{
    if (FModuleManager::Get().IsModuleLoaded(TEXT("Renderer")))
        FModuleManager::GetModuleChecked<IRendererModule>(TEXT("Renderer")).RemovePostOpaqueRenderDelegate(RenderHandle);
    ENQUEUE_RENDER_COMMAND(HUMAClothClear)([](FRHICommandListImmediate&) { States.Reset(); });
    FlushRenderingCommands();
}