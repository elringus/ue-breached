// Copyright 1998-2015 Epic Games, Inc. All Rights Reserved.

/*=============================================================================
	RHI.cpp: Render Hardware Interface implementation.
=============================================================================*/

#include "RHI.h"
#include "ModuleManager.h"

IMPLEMENT_MODULE(FDefaultModuleImpl, RHI);

/** RHI Logging. */
DEFINE_LOG_CATEGORY(LogRHI);

// Define counter stats.
DEFINE_STAT(STAT_RHIDrawPrimitiveCalls);
DEFINE_STAT(STAT_RHITriangles);
DEFINE_STAT(STAT_RHILines);

// Define memory stats.
DEFINE_STAT(STAT_RenderTargetMemory2D);
DEFINE_STAT(STAT_RenderTargetMemory3D);
DEFINE_STAT(STAT_RenderTargetMemoryCube);
DEFINE_STAT(STAT_TextureMemory2D);
DEFINE_STAT(STAT_TextureMemory3D);
DEFINE_STAT(STAT_TextureMemoryCube);
DEFINE_STAT(STAT_UniformBufferMemory);
DEFINE_STAT(STAT_IndexBufferMemory);
DEFINE_STAT(STAT_VertexBufferMemory);
DEFINE_STAT(STAT_StructuredBufferMemory);
DEFINE_STAT(STAT_PixelBufferMemory);

#if STATS
#include "StatsData.h"
static void DumpRHIMemory(FOutputDevice& OutputDevice)
{
	TArray<FStatMessage> Stats;
	GetPermanentStats(Stats);

	FName NAME_STATGROUP_RHI(FStatGroup_STATGROUP_RHI::GetGroupName());
	OutputDevice.Logf(TEXT("RHI resource memory (not tracked by our allocator)"));
	int64 TotalMemory = 0;
	for (int32 Index = 0; Index < Stats.Num(); Index++)
	{
		FStatMessage const& Meta = Stats[Index];
		FName LastGroup = Meta.NameAndInfo.GetGroupName();
		if (LastGroup == NAME_STATGROUP_RHI && Meta.NameAndInfo.GetFlag(EStatMetaFlags::IsMemory))
		{
			OutputDevice.Logf(TEXT("%s"), *FStatsUtils::DebugPrint(Meta));
			TotalMemory += Meta.GetValue_int64();
		}
	}
	OutputDevice.Logf(TEXT("%.3fMB total"), TotalMemory / 1024.f / 1024.f);
}

static FAutoConsoleCommandWithOutputDevice GDumpRHIMemoryCmd(
	TEXT("rhi.DumpMemory"),
	TEXT("Dumps RHI memory stats to the log"),
	FConsoleCommandWithOutputDeviceDelegate::CreateStatic(DumpRHIMemory)
	);
#endif

//DO NOT USE THE STATIC FLINEARCOLORS TO INITIALIZE THIS STUFF.  
//Static init order is undefined and you will likely end up with bad values on some platforms.
const FClearValueBinding FClearValueBinding::None(EClearBinding::ENoneBound);
const FClearValueBinding FClearValueBinding::Black(FLinearColor(0.0f, 0.0f, 0.0f, 1.0f));
const FClearValueBinding FClearValueBinding::White(FLinearColor(1.0f, 1.0f, 1.0f, 1.0f));
const FClearValueBinding FClearValueBinding::Transparent(FLinearColor(0.0f, 0.0f, 0.0f, 0.0f));
const FClearValueBinding FClearValueBinding::DepthOne(1.0f, 0);
const FClearValueBinding FClearValueBinding::DepthZero(0.0f, 0);
const FClearValueBinding FClearValueBinding::DepthNear((float)ERHIZBuffer::NearPlane, 0);
const FClearValueBinding FClearValueBinding::DepthFar((float)ERHIZBuffer::FarPlane, 0);


TLockFreePointerList<FRHIResource> FRHIResource::PendingDeletes;
FRHIResource* FRHIResource::CurrentlyDeleting = nullptr;

#if !DISABLE_RHI_DEFFERED_DELETE
bool FRHIResource::Bypass()
{
	return GRHICommandList.Bypass();
}
#endif

DECLARE_CYCLE_STAT(TEXT("Delete Resources"), STAT_DeleteResources, STATGROUP_RHICMDLIST);

void FRHIResource::FlushPendingDeletes()
{
	SCOPE_CYCLE_COUNTER(STAT_DeleteResources);

	check(IsInRenderingThread());
	FRHICommandListExecutor::CheckNoOutstandingCmdLists();
	FRHICommandListExecutor::GetImmediateCommandList().ImmediateFlush(EImmediateFlushType::FlushRHIThread);
	while (1)
	{
		TArray<FRHIResource*> ToDelete;
		PendingDeletes.PopAll(ToDelete);
		if (!ToDelete.Num())
		{
			break;
		}
		for (int32 Index = 0; Index < ToDelete.Num(); Index++)
		{
			FRHIResource* Ref = ToDelete[Index];
			check(Ref->MarkedForDelete == 1);
			if (Ref->GetRefCount() == 0) // caches can bring dead objects back to life
			{
				CurrentlyDeleting = Ref;
				delete Ref;
				CurrentlyDeleting = nullptr;
			}
			else
			{
				Ref->MarkedForDelete = 0;
				FPlatformMisc::MemoryBarrier();
			}
		}
	}
}


static_assert(ERHIZBuffer::FarPlane != ERHIZBuffer::NearPlane, "Near and Far planes must be different!");
static_assert((int32)ERHIZBuffer::NearPlane == 0 || (int32)ERHIZBuffer::NearPlane == 1, "Invalid Values for Near Plane, can only be 0 or 1!");
static_assert((int32)ERHIZBuffer::FarPlane == 0 || (int32)ERHIZBuffer::FarPlane == 1, "Invalid Values for Far Plane, can only be 0 or 1");


/**
 * RHI configuration settings.
 */

static TAutoConsoleVariable<int32> ResourceTableCachingCvar(
	TEXT("rhi.ResourceTableCaching"),
	1,
	TEXT("If 1, the RHI will cache resource table contents within a frame. Otherwise resource tables are rebuilt for every draw call.")
	);
static TAutoConsoleVariable<int32> GSaveScreenshotAfterProfilingGPUCVar(
	TEXT("r.ProfileGPU.Screenshot"),
	1,
	TEXT("Whether a screenshot should be taken when profiling the GPU. 0:off, 1:on (default)"),
	ECVF_RenderThreadSafe);
static TAutoConsoleVariable<int32> GShowProfilerAfterProfilingGPUCVar(
	TEXT("r.ProfileGPU.ShowUI"),
	1,
	TEXT("Whether the user interface profiler should be displayed after profiling the GPU.\n")
	TEXT("The results will always go to the log/console\n")
	TEXT("0:off, 1:on (default)"),
	ECVF_RenderThreadSafe);
static TAutoConsoleVariable<float> GGPUHitchThresholdCVar(
	TEXT("RHI.GPUHitchThreshold"),
	100.0f,
	TEXT("Threshold for detecting hitches on the GPU (in milliseconds).")
	);

namespace RHIConfig
{
	bool ShouldSaveScreenshotAfterProfilingGPU()
	{
		return GSaveScreenshotAfterProfilingGPUCVar.GetValueOnAnyThread() != 0;
	}

	bool ShouldShowProfilerAfterProfilingGPU()
	{
		return GShowProfilerAfterProfilingGPUCVar.GetValueOnAnyThread() != 0;
	}

	float GetGPUHitchThreshold()
	{
		return GGPUHitchThresholdCVar.GetValueOnAnyThread() * 0.001f;
	}
}

/**
 * RHI globals.
 */

bool GIsRHIInitialized = false;
int32 GMaxTextureMipCount = MAX_TEXTURE_MIP_COUNT;
bool GSupportsQuadBufferStereo = false;
bool GSupportsDepthFetchDuringDepthTest = true;
FString GRHIAdapterName;
uint32 GRHIVendorId = 0;
bool GSupportsRenderDepthTargetableShaderResources = true;
bool GSupportsRenderTargetFormat_PF_G8 = true;
bool GSupportsRenderTargetFormat_PF_FloatRGBA = true;
bool GSupportsShaderFramebufferFetch = false;
bool GSupportsShaderDepthStencilFetch = false;
bool GHardwareHiddenSurfaceRemoval = false;
bool GRHISupportsAsyncTextureCreation = false;
bool GSupportsQuads = false;
bool GSupportsVolumeTextureRendering = true;
bool GSupportsSeparateRenderTargetBlendState = false;
bool GSupportsDepthRenderTargetWithoutColorRenderTarget = true;
float GMinClipZ = 0.0f;
float GProjectionSignY = 1.0f;
int32 GMaxShadowDepthBufferSizeX = 2048;
int32 GMaxShadowDepthBufferSizeY = 2048;
int32 GMaxTextureDimensions = 2048;
int32 GMaxCubeTextureDimensions = 2048;
int32 GMaxTextureArrayLayers = 256;
bool GUsingNullRHI = false;
int32 GDrawUPVertexCheckCount = MAX_int32;
int32 GDrawUPIndexCheckCount = MAX_int32;
bool GTriggerGPUProfile = false;
bool GRHISupportsTextureStreaming = false;
bool GSupportsDepthBoundsTest = false;
bool GRHISupportsBaseVertexIndex = true;
bool GRHISupportsFirstInstance = false;
bool GRHIRequiresEarlyBackBufferRenderTarget = true;
bool GRHISupportsRHIThread = false;
bool GRHISupportsParallelRHIExecute = false;
bool GSupportsHDR32bppEncodeModeIntrinsic = false;


/** Whether we are profiling GPU hitches. */
bool GTriggerGPUHitchProfile = false;

#if WITH_SLI
int32 GNumActiveGPUsForRendering = 1;
#endif

FVertexElementTypeSupportInfo GVertexElementTypeSupport;

RHI_API int32 volatile GCurrentTextureMemorySize = 0;
RHI_API int32 volatile GCurrentRendertargetMemorySize = 0;
RHI_API int64 GTexturePoolSize = 0 * 1024 * 1024;
RHI_API int32 GPoolSizeVRAMPercentage = 0;

RHI_API EShaderPlatform GShaderPlatformForFeatureLevel[ERHIFeatureLevel::Num] = {SP_NumPlatforms,SP_NumPlatforms,SP_NumPlatforms,SP_NumPlatforms};

RHI_API int32 GNumDrawCallsRHI = 0;
RHI_API int32 GNumPrimitivesDrawnRHI = 0;

/** Called once per frame only from within an RHI. */
void RHIPrivateBeginFrame()
{
	GNumDrawCallsRHI = 0;
	GNumPrimitivesDrawnRHI = 0;
}

//
// The current shader platform.
//

RHI_API EShaderPlatform GMaxRHIShaderPlatform = SP_PCD3D_SM5;

/** The maximum feature level supported on this machine */
RHI_API ERHIFeatureLevel::Type GMaxRHIFeatureLevel = ERHIFeatureLevel::SM5;

FName FeatureLevelNames[] = 
{
	FName(TEXT("ES2")),
	FName(TEXT("ES3_1")),
	FName(TEXT("SM4")),
	FName(TEXT("SM5")),
};

static_assert(ARRAY_COUNT(FeatureLevelNames) == ERHIFeatureLevel::Num, "Missing entry from feature level names.");

RHI_API bool GetFeatureLevelFromName(FName Name, ERHIFeatureLevel::Type& OutFeatureLevel)
{
	for (int32 NameIndex = 0; NameIndex < ARRAY_COUNT(FeatureLevelNames); NameIndex++)
	{
		if (FeatureLevelNames[NameIndex] == Name)
		{
			OutFeatureLevel = (ERHIFeatureLevel::Type)NameIndex;
			return true;
		}
	}

	OutFeatureLevel = ERHIFeatureLevel::Num;
	return false;
}

RHI_API void GetFeatureLevelName(ERHIFeatureLevel::Type InFeatureLevel, FString& OutName)
{
	check(InFeatureLevel < ARRAY_COUNT(FeatureLevelNames));
	FeatureLevelNames[(int32)InFeatureLevel].ToString(OutName);
}

RHI_API void GetFeatureLevelName(ERHIFeatureLevel::Type InFeatureLevel, FName& OutName)
{
	check(InFeatureLevel < ARRAY_COUNT(FeatureLevelNames));
	OutName = FeatureLevelNames[(int32)InFeatureLevel];
}

static FName NAME_PCD3D_SM5(TEXT("PCD3D_SM5"));
static FName NAME_PCD3D_SM4(TEXT("PCD3D_SM4"));
static FName NAME_PCD3D_ES3_1(TEXT("PCD3D_ES31"));
static FName NAME_PCD3D_ES2(TEXT("PCD3D_ES2"));
static FName NAME_GLSL_150(TEXT("GLSL_150"));
static FName NAME_GLSL_150_MAC(TEXT("GLSL_150_MAC"));
static FName NAME_SF_PS4(TEXT("SF_PS4"));
static FName NAME_SF_XBOXONE(TEXT("SF_XBOXONE"));
static FName NAME_GLSL_430(TEXT("GLSL_430"));
static FName NAME_GLSL_150_ES2(TEXT("GLSL_150_ES2"));
static FName NAME_GLSL_150_ES2_NOUB(TEXT("GLSL_150_ES2_NOUB"));
static FName NAME_GLSL_150_ES31(TEXT("GLSL_150_ES31"));
static FName NAME_GLSL_ES2(TEXT("GLSL_ES2"));
static FName NAME_GLSL_ES2_WEBGL(TEXT("GLSL_ES2_WEBGL"));
static FName NAME_GLSL_ES2_IOS(TEXT("GLSL_ES2_IOS"));
static FName NAME_SF_METAL(TEXT("SF_METAL"));
static FName NAME_SF_METAL_MRT(TEXT("SF_METAL_MRT"));
static FName NAME_GLSL_310_ES_EXT(TEXT("GLSL_310_ES_EXT"));
static FName NAME_SF_METAL_SM5(TEXT("SF_METAL_SM5"));
static FName NAME_PC_VULKAN_ES2(TEXT("PC_VULKAN_ES2"));

FName LegacyShaderPlatformToShaderFormat(EShaderPlatform Platform)
{
	switch(Platform)
	{
	case SP_PCD3D_SM5:
		return NAME_PCD3D_SM5;
	case SP_PCD3D_SM4:
		return NAME_PCD3D_SM4;
	case SP_PCD3D_ES3_1:
		return NAME_PCD3D_ES3_1;
	case SP_PCD3D_ES2:
		return NAME_PCD3D_ES2;
	case SP_OPENGL_SM4:
		return NAME_GLSL_150;
	case SP_OPENGL_SM4_MAC:
		return NAME_GLSL_150_MAC;
	case SP_PS4:
		return NAME_SF_PS4;
	case SP_XBOXONE:
		return NAME_SF_XBOXONE;
	case SP_OPENGL_SM5:
		return NAME_GLSL_430;
	case SP_OPENGL_PCES2:
	{
		static auto* CVar = IConsoleManager::Get().FindTConsoleVariableDataInt(TEXT("OpenGL.UseEmulatedUBs"));
		return (CVar && CVar->GetValueOnAnyThread() != 0) ? NAME_GLSL_150_ES2_NOUB : NAME_GLSL_150_ES2;
	}
	case SP_OPENGL_PCES3_1:
		return NAME_GLSL_150_ES31;
	case SP_OPENGL_ES2_ANDROID:
		return NAME_GLSL_ES2;
	case SP_OPENGL_ES2_WEBGL:
		return NAME_GLSL_ES2_WEBGL;
	case SP_OPENGL_ES2_IOS:
		return NAME_GLSL_ES2_IOS;
	case SP_METAL:
		return NAME_SF_METAL;
	case SP_METAL_MRT:
		return NAME_SF_METAL_MRT;
	case SP_METAL_SM5:
		return NAME_SF_METAL_SM5;
	case SP_OPENGL_ES31_EXT:
		return NAME_GLSL_310_ES_EXT;
	case SP_VULKAN_ES2:
		return NAME_PC_VULKAN_ES2;

	default:
		check(0);
		return NAME_PCD3D_SM5;
	}
}

EShaderPlatform ShaderFormatToLegacyShaderPlatform(FName ShaderFormat)
{
	if (ShaderFormat == NAME_PCD3D_SM5)			return SP_PCD3D_SM5;	
	if (ShaderFormat == NAME_PCD3D_SM4)			return SP_PCD3D_SM4;
	if (ShaderFormat == NAME_PCD3D_ES3_1)		return SP_PCD3D_ES3_1;
	if (ShaderFormat == NAME_PCD3D_ES2)			return SP_PCD3D_ES2;
	if (ShaderFormat == NAME_GLSL_150)			return SP_OPENGL_SM4;
	if (ShaderFormat == NAME_GLSL_150_MAC)		return SP_OPENGL_SM4_MAC;
	if (ShaderFormat == NAME_SF_PS4)				return SP_PS4;
	if (ShaderFormat == NAME_SF_XBOXONE)			return SP_XBOXONE;
	if (ShaderFormat == NAME_GLSL_430)			return SP_OPENGL_SM5;
	if (ShaderFormat == NAME_GLSL_150_ES2 || ShaderFormat == NAME_GLSL_150_ES2_NOUB)
												return SP_OPENGL_PCES2;
	if (ShaderFormat == NAME_GLSL_150_ES31)		return SP_OPENGL_PCES3_1;
	if (ShaderFormat == NAME_GLSL_ES2)			return SP_OPENGL_ES2_ANDROID;
	if (ShaderFormat == NAME_GLSL_ES2_WEBGL)	return SP_OPENGL_ES2_WEBGL;
	if (ShaderFormat == NAME_GLSL_ES2_IOS)		return SP_OPENGL_ES2_IOS;
	if (ShaderFormat == NAME_SF_METAL)			return SP_METAL;
	if (ShaderFormat == NAME_SF_METAL_MRT)		return SP_METAL_MRT;
	if (ShaderFormat == NAME_GLSL_310_ES_EXT)	return SP_OPENGL_ES31_EXT;
	if (ShaderFormat == NAME_SF_METAL_SM5)		return SP_METAL_SM5;
	if (ShaderFormat == NAME_PC_VULKAN_ES2)		return SP_VULKAN_ES2;
	return SP_NumPlatforms;
}


RHI_API bool IsRHIDeviceAMD()
{
	check(GRHIVendorId != 0);
	// AMD's drivers tested on July 11 2013 have hitching problems with async resource streaming, setting single threaded for now until fixed.
	return GRHIVendorId == 0x1002;
}

RHI_API bool IsRHIDeviceIntel()
{
	check(GRHIVendorId != 0);
	// Intel GPUs are integrated and use both DedicatedVideoMemory and SharedSystemMemory.
	// The hardware has fast clears so we disable exclude rects (see r.ClearWithExcludeRects)
	return GRHIVendorId == 0x8086;
}

RHI_API bool IsRHIDeviceNVIDIA()
{
	check(GRHIVendorId != 0);
	// NVIDIA GPUs are discrete and use DedicatedVideoMemory only.
	return GRHIVendorId == 0x10DE;
}
