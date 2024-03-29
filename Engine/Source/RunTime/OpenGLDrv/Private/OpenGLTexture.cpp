// Copyright Epic Games, Inc. All Rights Reserved.

/*=============================================================================
	OpenGLTexture.cpp: OpenGL texture RHI implementation.
=============================================================================*/

#include "CoreMinimal.h"
#include "Containers/ResourceArray.h"
#include "Stats/Stats.h"
#include "RHI.h"
#include "RenderUtils.h"
#include "OpenGLDrv.h"
#include "OpenGLDrvPrivate.h"
#include "HAL/LowLevelMemTracker.h"
#include "Engine/Texture.h"

#if PLATFORM_ANDROID
#include "ThirdParty/Android/detex/AndroidETC.h"
#endif //PLATFORM_ANDROID

//CBR Code
static TAutoConsoleVariable<int32> CVarTileMem(
	TEXT("r.OpenGL.UseTileMem"),
	0,
	TEXT("0: TileMem should not be use with mobile CBR(default)\n")
	TEXT("1: Setting to true may cause problem on mobile with CBR\n"),
	ECVF_RenderThreadSafe);
//

static TAutoConsoleVariable<int32> CVarDeferTextureCreation(
	TEXT("r.OpenGL.DeferTextureCreation"),
	0,
	TEXT("0: OpenGL textures are sent to the driver to be created immediately. (default)\n")
	TEXT("1: Where possible OpenGL textures are stored in system memory and created only when required for rendering.\n")
	TEXT("   This can avoid memory overhead seen in some GL drivers."),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarDeferTextureCreationExcludeMask(
	TEXT("r.OpenGL.DeferTextureCreationExcludeFlags"),
	~(TexCreate_ShaderResource | TexCreate_SRGB | TexCreate_Transient | TexCreate_Streamable | TexCreate_OfflineProcessed),
	TEXT("Deferred texture creation exclusion mask, any texture requested with flags in this mask will be excluded from deferred creation."),
	ECVF_RenderThreadSafe);

static int32 GOGLDeferTextureCreationKeepLowerMipCount = -1;
static FAutoConsoleVariableRef CVarDeferTextureCreationKeepLowerMipCount(
	TEXT("r.OpenGL.DeferTextureCreationKeepLowerMipCount"),
	GOGLDeferTextureCreationKeepLowerMipCount,
	TEXT("Maximum number of texture mips to retain in CPU memory after a deferred texture has been sent to the driver for GPU memory creation.\n")
	TEXT("-1: to match the number of mips kept resident by the texture streamer (default).\n")
	TEXT(" 0: to disable texture eviction and discard CPU mips after sending them to the driver.\n")
	TEXT(" 16: keep all mips around.\n"),
	ECVF_RenderThreadSafe);

static int32 GOGLTextureEvictFramesToLive = 500;
static FAutoConsoleVariableRef CVarTextureEvictionFrameCount(
	TEXT("r.OpenGL.TextureEvictionFrameCount"),
	GOGLTextureEvictFramesToLive,
	TEXT("The number of frames since a texture was last referenced before it will considered for eviction.\n")
	TEXT("Textures can only be evicted after creation if all their mips are resident, ie its mip count <= r.OpenGL.DeferTextureCreationKeepLowerMipCount."),
	ECVF_RenderThreadSafe
);

int32 GOGLTexturesToEvictPerFrame = 10;
static FAutoConsoleVariableRef CVarTexturesToEvictPerFrame(
	TEXT("r.OpenGL.TextureEvictsPerFrame"),
	GOGLTexturesToEvictPerFrame,
	TEXT("The maximum number of evictable textures to evict per frame, limited to avoid potential driver CPU spikes.\n")
	TEXT("Textures can only be evicted after creation if all their mips are resident, ie its mip count <= r.OpenGL.DeferTextureCreationKeepLowerMipCount."),
	ECVF_RenderThreadSafe
);

static int32 GOGLTextureEvictLogging = 0;
static FAutoConsoleVariableRef CVarTextureEvictionLogging(
	TEXT("r.OpenGL.TextureEvictionLogging"),
	GOGLTextureEvictLogging,
	TEXT("Enables debug logging for texture eviction."),
	ECVF_RenderThreadSafe
);

class FOpenGLDynamicRHI* FOpenGLTextureBase::OpenGLRHI = nullptr;

/*-----------------------------------------------------------------------------
	Texture allocator support.
-----------------------------------------------------------------------------*/

/** Caching it here, to avoid getting it every time we create a texture. 0 is no multisampling. */
GLint GMaxOpenGLColorSamples = 2;//CBR Code set to 2 but default 0
GLint GMaxOpenGLDepthSamples = 2;
GLint GMaxOpenGLIntegerSamples = 2;

// in bytes, never change after RHI, needed to scale game features
int64 GOpenGLDedicatedVideoMemory = 0;
// In bytes. Never changed after RHI init. Our estimate of the amount of memory that we can use for graphics resources in total.
int64 GOpenGLTotalGraphicsMemory = 0;

static bool ShouldCountAsTextureMemory(ETextureCreateFlags Flags)
{
	return (Flags & (TexCreate_RenderTargetable | TexCreate_ResolveTargetable | TexCreate_DepthStencilTargetable)) == 0;
}

void OpenGLTextureAllocated(FRHITexture* Texture, ETextureCreateFlags Flags)
{
	int32 TextureSize = 0;
	FOpenGLTextureCube* TextureCube = 0;
	FOpenGLTexture2D* Texture2D = 0;
	FOpenGLTexture2DArray* Texture2DArray = 0;
	FOpenGLTexture3D* Texture3D = 0;
	bool bRenderTarget = !ShouldCountAsTextureMemory(Flags);

	if (( TextureCube = (FOpenGLTextureCube*)Texture->GetTextureCube()) != NULL)
	{
		if (TextureCube->IsMemorySizeSet())
		{
			return; // already set this up on RT
		}

		TextureSize = CalcTextureSize( TextureCube->GetSize(), TextureCube->GetSize(), TextureCube->GetFormat(), TextureCube->GetNumMips() );
		TextureSize *= TextureCube->GetArraySize() * (TextureCube->GetArraySize() == 1 ? 6 : 1);
		TextureCube->SetMemorySize( TextureSize );
		TextureCube->SetIsPowerOfTwo(FMath::IsPowerOfTwo(TextureCube->GetSizeX()) && FMath::IsPowerOfTwo(TextureCube->GetSizeY()));
		if (bRenderTarget)
		{
			INC_MEMORY_STAT_BY(STAT_RenderTargetMemoryCube,TextureSize);
		}
		else
		{
			INC_MEMORY_STAT_BY(STAT_TextureMemoryCube,TextureSize);
		}
	}
	else if ((Texture2D = (FOpenGLTexture2D*)Texture->GetTexture2D()) != NULL)
	{
		if (Texture2D->IsMemorySizeSet())
		{
			return; // already set this up on RT
		}
		TextureSize = CalcTextureSize( Texture2D->GetSizeX(), Texture2D->GetSizeY(), Texture2D->GetFormat(), Texture2D->GetNumMips() )*Texture2D->GetNumSamples();
		Texture2D->SetMemorySize( TextureSize );
		Texture2D->SetIsPowerOfTwo(FMath::IsPowerOfTwo(Texture2D->GetSizeX()) && FMath::IsPowerOfTwo(Texture2D->GetSizeY()));
		if (bRenderTarget)
		{
			INC_MEMORY_STAT_BY(STAT_RenderTargetMemory2D,TextureSize);
		}
		else
		{
			INC_MEMORY_STAT_BY(STAT_TextureMemory2D,TextureSize);
		}
	}
	else if ((Texture3D = (FOpenGLTexture3D*)Texture->GetTexture3D()) != NULL)
	{
		if (Texture3D->IsMemorySizeSet())
		{
			return; // already set this up on RT
		}
		TextureSize = CalcTextureSize3D( Texture3D->GetSizeX(), Texture3D->GetSizeY(), Texture3D->GetSizeZ(), Texture3D->GetFormat(), Texture3D->GetNumMips() );
		Texture3D->SetMemorySize( TextureSize );
		Texture3D->SetIsPowerOfTwo(FMath::IsPowerOfTwo(Texture3D->GetSizeX()) && FMath::IsPowerOfTwo(Texture3D->GetSizeY()) && FMath::IsPowerOfTwo(Texture3D->GetSizeZ()));
		if (bRenderTarget)
		{
			INC_MEMORY_STAT_BY(STAT_RenderTargetMemory3D,TextureSize);
		}
		else
		{
			INC_MEMORY_STAT_BY(STAT_TextureMemory3D,TextureSize);
		}
	}
	else if ((Texture2DArray = (FOpenGLTexture2DArray*)Texture->GetTexture2DArray()) != NULL)
	{
		if (Texture2DArray->IsMemorySizeSet())
		{
			return; // already set this up on RT
		}
		TextureSize = Texture2DArray->GetSizeZ() * CalcTextureSize( Texture2DArray->GetSizeX(), Texture2DArray->GetSizeY(), Texture2DArray->GetFormat(), Texture2DArray->GetNumMips() );
		Texture2DArray->SetMemorySize( TextureSize );
		Texture2DArray->SetIsPowerOfTwo(FMath::IsPowerOfTwo(Texture2DArray->GetSizeX()) && FMath::IsPowerOfTwo(Texture2DArray->GetSizeY()));
		if (bRenderTarget)
		{
			INC_MEMORY_STAT_BY(STAT_RenderTargetMemory2D,TextureSize);
		}
		else
		{
			INC_MEMORY_STAT_BY(STAT_TextureMemory2D,TextureSize);
		}
	}
	else
	{
		check(0);	// Add handling of other texture types
	}

	if( bRenderTarget )
	{
		GCurrentRendertargetMemorySize += Align(TextureSize, 1024) / 1024;
#if ENABLE_LOW_LEVEL_MEM_TRACKER
		LLM_SCOPED_PAUSE_TRACKING_WITH_ENUM_AND_AMOUNT(ELLMTag::GraphicsPlatform, TextureSize, ELLMTracker::Platform, ELLMAllocType::None);
		LLM_SCOPED_PAUSE_TRACKING_WITH_ENUM_AND_AMOUNT(ELLMTag::RenderTargets, TextureSize, ELLMTracker::Default, ELLMAllocType::None);
#endif
	}
	else
	{
		GCurrentTextureMemorySize += Align(TextureSize, 1024) / 1024;
#if ENABLE_LOW_LEVEL_MEM_TRACKER
		LLM_SCOPED_PAUSE_TRACKING_WITH_ENUM_AND_AMOUNT(ELLMTag::GraphicsPlatform, TextureSize, ELLMTracker::Platform, ELLMAllocType::None);
		LLM_SCOPED_PAUSE_TRACKING_WITH_ENUM_AND_AMOUNT(ELLMTag::Textures, TextureSize, ELLMTracker::Default, ELLMAllocType::None);
#endif
	}
}

void OpenGLTextureDeleted( FRHITexture* Texture )
{
	bool bRenderTarget = !ShouldCountAsTextureMemory(Texture->GetFlags());
	int32 TextureSize = 0;
	if (Texture->GetTextureCube())
	{
		TextureSize = ((FOpenGLTextureCube*)Texture->GetTextureCube())->GetMemorySize();
		if (bRenderTarget)
		{
			DEC_MEMORY_STAT_BY(STAT_RenderTargetMemoryCube,TextureSize);
		}
		else
		{
			DEC_MEMORY_STAT_BY(STAT_TextureMemoryCube,TextureSize);
		}
	}
	else if (Texture->GetTexture2D())
	{
		TextureSize = ((FOpenGLTexture2D*)Texture->GetTexture2D())->GetMemorySize();
		if (bRenderTarget)
		{
			DEC_MEMORY_STAT_BY(STAT_RenderTargetMemory2D,TextureSize);
		}
		else
		{
			DEC_MEMORY_STAT_BY(STAT_TextureMemory2D,TextureSize);
		}
	}
	else if (Texture->GetTexture3D())
	{
		TextureSize = ((FOpenGLTexture3D*)Texture->GetTexture3D())->GetMemorySize();
		if (bRenderTarget)
		{
			DEC_MEMORY_STAT_BY(STAT_RenderTargetMemory3D,TextureSize);
		}
		else
		{
			DEC_MEMORY_STAT_BY(STAT_TextureMemory3D,TextureSize);
		}
	}
	else if (Texture->GetTexture2DArray())
	{
		TextureSize = ((FOpenGLTexture2DArray*)Texture->GetTexture2DArray())->GetMemorySize();
		if (bRenderTarget)
		{
			DEC_MEMORY_STAT_BY(STAT_RenderTargetMemory2D,TextureSize);
		}
		else
		{
			DEC_MEMORY_STAT_BY(STAT_TextureMemory2D,TextureSize);
		}
	}
	else
	{
		check(0);	// Add handling of other texture types
	}

	if( bRenderTarget )
	{
		GCurrentRendertargetMemorySize -= Align(TextureSize, 1024) / 1024;
#if ENABLE_LOW_LEVEL_MEM_TRACKER
		LLM_SCOPED_PAUSE_TRACKING_WITH_ENUM_AND_AMOUNT(ELLMTag::GraphicsPlatform, -TextureSize, ELLMTracker::Platform, ELLMAllocType::None);
		LLM_SCOPED_PAUSE_TRACKING_WITH_ENUM_AND_AMOUNT(ELLMTag::RenderTargets, -TextureSize, ELLMTracker::Default, ELLMAllocType::None);
#endif
	}
	else
	{
		GCurrentTextureMemorySize -= Align(TextureSize, 1024) / 1024;
#if ENABLE_LOW_LEVEL_MEM_TRACKER
		LLM_SCOPED_PAUSE_TRACKING_WITH_ENUM_AND_AMOUNT(ELLMTag::GraphicsPlatform, -TextureSize, ELLMTracker::Platform, ELLMAllocType::None);
		LLM_SCOPED_PAUSE_TRACKING_WITH_ENUM_AND_AMOUNT(ELLMTag::Textures, -TextureSize, ELLMTracker::Default, ELLMAllocType::None);
#endif
	}
}

uint64 FOpenGLDynamicRHI::RHICalcTexture2DPlatformSize(uint32 SizeX, uint32 SizeY, uint8 Format, uint32 NumMips, uint32 NumSamples, ETextureCreateFlags Flags, const FRHIResourceCreateInfo& CreateInfo, uint32& OutAlign)
{
	OutAlign = 0;
	return CalcTextureSize(SizeX, SizeY, (EPixelFormat)Format, NumMips);
}

uint64 FOpenGLDynamicRHI::RHICalcTexture3DPlatformSize(uint32 SizeX, uint32 SizeY, uint32 SizeZ, uint8 Format, uint32 NumMips, ETextureCreateFlags Flags, const FRHIResourceCreateInfo& CreateInfo, uint32& OutAlign)
{
	OutAlign = 0;
	return CalcTextureSize3D(SizeX, SizeY, SizeZ, (EPixelFormat)Format, NumMips);
}

uint64 FOpenGLDynamicRHI::RHICalcTextureCubePlatformSize(uint32 Size, uint8 Format, uint32 NumMips, ETextureCreateFlags Flags, const FRHIResourceCreateInfo& CreateInfo, uint32& OutAlign)
{
	OutAlign = 0;
	return CalcTextureSize(Size, Size, (EPixelFormat)Format, NumMips) * 6;
}

/**
 * Retrieves texture memory stats. Unsupported with this allocator.
 *
 * @return false, indicating that out variables were left unchanged.
 */
void FOpenGLDynamicRHI::RHIGetTextureMemoryStats(FTextureMemoryStats& OutStats)
{
	OutStats.DedicatedVideoMemory = GOpenGLDedicatedVideoMemory;
    OutStats.DedicatedSystemMemory = 0;
    OutStats.SharedSystemMemory = 0;
	OutStats.TotalGraphicsMemory = GOpenGLTotalGraphicsMemory ? GOpenGLTotalGraphicsMemory : -1;

	OutStats.AllocatedMemorySize = int64(GCurrentTextureMemorySize) * 1024;
	OutStats.LargestContiguousAllocation = OutStats.AllocatedMemorySize;
	OutStats.TexturePoolSize = GTexturePoolSize;
	OutStats.PendingMemoryAdjustment = 0;
}


/**
 * Fills a texture with to visualize the texture pool memory.
 *
 * @param	TextureData		Start address
 * @param	SizeX			Number of pixels along X
 * @param	SizeY			Number of pixels along Y
 * @param	Pitch			Number of bytes between each row
 * @param	PixelSize		Number of bytes each pixel represents
 *
 * @return true if successful, false otherwise
 */
bool FOpenGLDynamicRHI::RHIGetTextureMemoryVisualizeData( FColor* /*TextureData*/, int32 /*SizeX*/, int32 /*SizeY*/, int32 /*Pitch*/, int32 /*PixelSize*/ )
{
	return false;
}


FRHITexture* FOpenGLDynamicRHI::CreateOpenGLTexture(uint32 SizeX, uint32 SizeY, bool bCubeTexture, bool bArrayTexture, bool bIsExternal, uint8 Format, uint32 NumMips, uint32 NumSamples, uint32 ArraySize, ETextureCreateFlags Flags, const FClearValueBinding& InClearValue, FResourceBulkDataInterface* BulkData)
{
	// Fill in the GL resources.
	FRHITexture* Texture = CreateOpenGLRHITextureOnly(SizeX, SizeY, bCubeTexture, bArrayTexture, bIsExternal, Format, NumMips, NumSamples, ArraySize, Flags, InClearValue, BulkData);

	InitializeGLTexture(Texture, SizeX, SizeY, bCubeTexture, bArrayTexture, bIsExternal, Format, NumMips, NumSamples, ArraySize, Flags, InClearValue, BulkData);
	return Texture;
}

// Allocate only the RHIresource and its initialize FRHITexture's state.
// note this can change the value of some input parameters.
FRHITexture* FOpenGLDynamicRHI::CreateOpenGLRHITextureOnly(const uint32 SizeX, const uint32 SizeY, const bool bCubeTexture, const bool bArrayTexture, const bool bIsExternal, uint8& Format, uint32& NumMips, uint32& NumSamples, const uint32 ArraySize, ETextureCreateFlags Flags, const FClearValueBinding& InClearValue, FResourceBulkDataInterface* BulkData)
{
	SCOPE_CYCLE_COUNTER(STAT_OpenGLCreateTextureTime);

	if (NumMips == 0)
	{
		if (NumSamples <= 1)
		{
			NumMips = FindMaxMipmapLevel(SizeX, SizeY);
		}
		else
		{
			NumMips = 1;
		}
	}

#if UE_BUILD_DEBUG
	check(!(NumSamples > 1 && bCubeTexture));
	check(bArrayTexture != (ArraySize == 1));
#endif

	// Move NumSamples to on-chip MSAA if supported
	uint32 NumSamplesTileMem = 1;
	GLint MaxSamplesTileMem = FOpenGL::GetMaxMSAASamplesTileMem(); /* RHIs which do not support tiled GPU MSAA return 0 */
	
	if (MaxSamplesTileMem > 0 && CVarTileMem.GetValueOnRenderThread())//CBR Code 不使用TileMem
	{
		NumSamplesTileMem = FMath::Min<uint32>(NumSamples, MaxSamplesTileMem);
		NumSamples = 1;
	}

	GLenum Target = GL_NONE;
	if (bCubeTexture)
	{
		if (FOpenGL::SupportsTexture3D())
		{
			Target = bArrayTexture ? GL_TEXTURE_CUBE_MAP_ARRAY : GL_TEXTURE_CUBE_MAP;
		}
		else
		{
			check(!bArrayTexture);
			Target = GL_TEXTURE_CUBE_MAP;
		}
		check(SizeX == SizeY);
	}
#if PLATFORM_ANDROID && !PLATFORM_LUMINGL4
	else if (bIsExternal)//false
	{
		if (FOpenGL::SupportsImageExternal())
		{
			Target = GL_TEXTURE_EXTERNAL_OES;
		}
		else
		{
			// Fall back to a regular 2d texture if we don't have support. Texture samplers in the shader will also fall back to a regular sampler2D.
			Target = GL_TEXTURE_2D;
		}
	}
#endif
	else
	{
		Target =  (NumSamples > 1) ? GL_TEXTURE_2D_MULTISAMPLE : GL_TEXTURE_2D;

		// @todo: refactor 2d texture array support here?
		check(!bArrayTexture);
	}
	check(Target != GL_NONE);


	FRHITexture* Result;
	// Allocate RHIResource with empty GL values.
	if (bCubeTexture)
	{
		Result = new FOpenGLTextureCube(this, 0, Target, -1, SizeX, SizeY, 0, NumMips, 1, 1, ArraySize, (EPixelFormat)Format, true, false, Flags, InClearValue);
	}
	else
	{
		Result = new FOpenGLTexture2D(this, 0, Target, -1, SizeX, SizeY, 0, NumMips, NumSamples, NumSamplesTileMem, 1, (EPixelFormat)Format, false, false, Flags, InClearValue);
	}
	OpenGLTextureAllocated(Result, Flags);

	check(!GetOpenGLTextureFromRHITexture(Result)->EvictionParamsPtr.IsValid());

	if (GetOpenGLTextureFromRHITexture(Result)->CanCreateAsEvicted())
	{
		GetOpenGLTextureFromRHITexture(Result)->EvictionParamsPtr = MakeUnique<FTextureEvictionParams>(NumMips);
	}

	return Result;
}

uint32 GTotalTexStorageSkipped = 0;
uint32 GTotalCompressedTexStorageSkipped = 0;
// Initalize the FRHITexture's GL resources and fill in state.
void FOpenGLDynamicRHI::InitializeGLTexture(FRHITexture* Texture, uint32 SizeX, const uint32 SizeY, const bool bCubeTexture, const bool bArrayTexture, const bool bIsExternal, const uint8 Format, const uint32 NumMips, const uint32 NumSamples, const uint32 ArraySize, const ETextureCreateFlags Flags, const FClearValueBinding& InClearValue, FResourceBulkDataInterface* BulkData)
{
	VERIFY_GL_SCOPE();

	const uint32 NumSamplesTileMem = bCubeTexture ? 1 : ((FOpenGLTexture2D*)Texture)->GetNumSamplesTileMem();
	const bool TileMemDepth = NumSamplesTileMem > 1 && (Flags & TexCreate_DepthStencilTargetable);

	GLuint TextureID = 0;
	if (!TileMemDepth)
	{
#if OPENGL_ES == 1
		//CBR Code
		const char* GLVersion = (const char*)glGetString(GL_VERSION);
		if (strstr(GLVersion, "OpenGL ES 3.2") && NumSamples == 2) {
			glEnable(GL_SAMPLE_SHADING_OES);
		}
		//CBR Code
#endif // OPENGL_ES
		FOpenGL::GenTextures(1, &TextureID);
	}
	if (!GetOpenGLTextureFromRHITexture(Texture)->IsEvicted())
	{
		InitializeGLTextureInternal(TextureID, Texture, SizeX, SizeY, bCubeTexture, bArrayTexture, bIsExternal, Format, NumMips, NumSamples, ArraySize, Flags, InClearValue, BulkData);
	}
	else
	{
		// creating this as 'evicted'.
		GTotalTexStorageSkipped++;
		//check(!GetOpenGLTextureFromRHITexture(Texture)->IsEvicted());
		{
			EPixelFormat PixelFormat = Texture->GetFormat();
			const FOpenGLTextureFormat& GLFormat = GOpenGLTextureFormats[PixelFormat];
			bool bIsCompressed = GLFormat.bCompressed;
			GTotalCompressedTexStorageSkipped += bIsCompressed ? 1 : 0;

			if(BulkData)
			{

				check(!GLFormat.bCompressed);
				const uint32 BlockSizeX = GPixelFormats[Format].BlockSizeX;
				const uint32 BlockSizeY = GPixelFormats[Format].BlockSizeY;
				uint8* Data = (uint8*)BulkData->GetResourceBulkData();
				uint32 MipOffset = 0;
				// copy bulk data to evicted mip store:
				for (uint32 MipIndex = 0; MipIndex < NumMips; MipIndex++)
				{
					uint32 NumBlocksX = AlignArbitrary(FMath::Max<uint32>(1, (SizeX >> MipIndex)), BlockSizeX) / BlockSizeX;
					uint32 NumBlocksY = AlignArbitrary(FMath::Max<uint32>(1, (SizeY >> MipIndex)), BlockSizeY) / BlockSizeY;
					uint32 NumLayers = FMath::Max<uint32>(1, ArraySize);
					uint32 MipDataSize = NumBlocksX * NumBlocksY * NumLayers * GPixelFormats[Format].BlockBytes;

					GetOpenGLTextureFromRHITexture(Texture)->EvictionParamsPtr->SetMipData(MipIndex, &Data[MipOffset],MipDataSize);
					MipOffset += MipDataSize;
				}
				BulkData->Discard();
			}
		}
		GetOpenGLTextureFromRHITexture(Texture)->SetResource(TextureID);
	}
}

static inline bool IsAstcLdrRGBAFormat(GLenum Format)
{
	return Format >= GL_COMPRESSED_RGBA_ASTC_4x4_KHR && Format <= GL_COMPRESSED_RGBA_ASTC_12x12_KHR;
}

void FOpenGLDynamicRHI::InitializeGLTextureInternal(GLuint TextureID, FRHITexture* Texture, uint32 SizeX, const uint32 SizeY, const bool bCubeTexture, const bool bArrayTexture, const bool bIsExternal, const uint8 Format, const uint32 NumMips, const uint32 NumSamples, const uint32 ArraySize, const ETextureCreateFlags Flags, const FClearValueBinding& InClearValue, FResourceBulkDataInterface* BulkData)
{

	VERIFY_GL_SCOPE();

	bool bAllocatedStorage = false;

	GLenum Target = bCubeTexture ? ((FOpenGLTextureCube*)Texture)->Target : ((FOpenGLTexture2D*)Texture)->Target;
	const uint32 NumSamplesTileMem = bCubeTexture ? 1 : ((FOpenGLTexture2D*)Texture)->GetNumSamplesTileMem();
	const bool TileMemDepth = NumSamplesTileMem > 1 && (Flags & TexCreate_DepthStencilTargetable);
	const bool bDepth = !TileMemDepth && (Flags & TexCreate_DepthStencilTargetable);//CBR Code

	check(TextureID || TileMemDepth);

	const bool bSRGB = (Flags & TexCreate_SRGB) != 0;
	const FOpenGLTextureFormat& GLFormat = GOpenGLTextureFormats[Format];
	if (GLFormat.InternalFormat[bSRGB] == GL_NONE)
	{
		UE_LOG(LogRHI, Fatal,TEXT("Texture format '%s' not supported (sRGB=%d)."), GPixelFormats[Format].Name, bSRGB);
	}

	FOpenGLContextState& ContextState = GetContextStateForCurrentContext();

	// Make sure PBO is disabled
	CachedBindPixelUnpackBuffer(ContextState,0);

	// Use a texture stage that's not likely to be used for draws, to avoid waiting
	CachedSetupTextureStage(ContextState, FOpenGL::GetMaxCombinedTextureImageUnits() - 1, Target, TextureID, 0, NumMips);

	if (NumSamples == 1 && !TileMemDepth)
	{
		if (Target == GL_TEXTURE_EXTERNAL_OES || !FMath::IsPowerOfTwo(SizeX) || !FMath::IsPowerOfTwo(SizeY))
		{
			glTexParameteri(Target, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
			glTexParameteri(Target, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
			if ( FOpenGL::SupportsTexture3D() )
			{
				glTexParameteri(Target, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
			}
		}
		else
		{
			glTexParameteri(Target, GL_TEXTURE_WRAP_S, GL_REPEAT);
			glTexParameteri(Target, GL_TEXTURE_WRAP_T, GL_REPEAT);
			if ( FOpenGL::SupportsTexture3D() )
			{
				glTexParameteri(Target, GL_TEXTURE_WRAP_R, GL_REPEAT);
			}
		}
		glTexParameteri(Target, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		glTexParameteri(Target, GL_TEXTURE_MIN_FILTER, NumMips > 1 ? GL_NEAREST_MIPMAP_NEAREST : GL_NEAREST);
		if( FOpenGL::SupportsTextureFilterAnisotropic() )
		{
			glTexParameteri(Target, GL_TEXTURE_MAX_ANISOTROPY_EXT, 1);
		}
		
		glTexParameteri(Target, GL_TEXTURE_BASE_LEVEL, 0);
	
#if PLATFORM_ANDROID && !PLATFORM_LUMINGL4
		// Do not use GL_TEXTURE_MAX_LEVEL if external texture on Android
		if (Target != GL_TEXTURE_EXTERNAL_OES)
#endif
		{
			glTexParameteri(Target, GL_TEXTURE_MAX_LEVEL, NumMips - 1);
		}
		
		TextureMipLimits.Add(TextureID, TPair<GLenum, GLenum>(0, NumMips - 1));
		
		if (GLFormat.bBGRA && !(Flags & TexCreate_RenderTargetable))
		{
			glTexParameteri(Target, GL_TEXTURE_SWIZZLE_R, GL_BLUE);
			glTexParameteri(Target, GL_TEXTURE_SWIZZLE_B, GL_RED);
		}

		if (FOpenGL::SupportsASTCDecodeMode())
		{
			if (IsAstcLdrRGBAFormat(GLFormat.InternalFormat[bSRGB]))
			{
				glTexParameteri(Target, TEXTURE_ASTC_DECODE_PRECISION_EXT, GL_RGBA8);
			}
		}

		if (bArrayTexture)
		{
			FOpenGL::TexStorage3D( Target, NumMips, GLFormat.InternalFormat[bSRGB], SizeX, SizeY, ArraySize, GLFormat.Format, GLFormat.Type);
		}
		else if (Target != GL_TEXTURE_EXTERNAL_OES)
		{
			// Try to allocate using TexStorage2D
			if (FOpenGL::TexStorage2D(Target, NumMips, GLFormat.SizedInternalFormat[bSRGB], SizeX, SizeY, GLFormat.Format, GLFormat.Type, Flags))
			{
				bAllocatedStorage = true;
			}
			else if (!GLFormat.bCompressed)
			{
				// Otherwise, allocate storage for each mip using TexImage2D
				// We can't do so for compressed textures because we can't pass NULL in to CompressedTexImage2D!
				bAllocatedStorage = true;

				const bool bIsCubeTexture = Target == GL_TEXTURE_CUBE_MAP;
				const GLenum FirstTarget = bIsCubeTexture ? GL_TEXTURE_CUBE_MAP_POSITIVE_X : Target;
				const uint32 NumTargets = bIsCubeTexture ? 6 : 1;

				for(uint32 MipIndex = 0; MipIndex < uint32(NumMips); MipIndex++)
				{
					for(uint32 TargetIndex = 0; TargetIndex < NumTargets; TargetIndex++)
					{
						glTexImage2D(
							FirstTarget + TargetIndex,
							MipIndex,
							GLFormat.InternalFormat[bSRGB],
							FMath::Max<uint32>(1,(SizeX >> MipIndex)),
							FMath::Max<uint32>(1,(SizeY >> MipIndex)),
							0,
							GLFormat.Format,
							GLFormat.Type,
							NULL
							);
					}
				}
			}
		}

		if (BulkData != NULL)
		{
			uint8* Data = (uint8*)BulkData->GetResourceBulkData();
			uint32 MipOffset = 0;

			const uint32 BlockSizeX = GPixelFormats[Format].BlockSizeX;
			const uint32 BlockSizeY = GPixelFormats[Format].BlockSizeY;
			for(uint32 MipIndex = 0; MipIndex < NumMips; MipIndex++)
			{
				uint32 NumBlocksX = AlignArbitrary(FMath::Max<uint32>(1,(SizeX >> MipIndex)), BlockSizeX) / BlockSizeX;
				uint32 NumBlocksY = AlignArbitrary(FMath::Max<uint32>(1,(SizeY >> MipIndex)), BlockSizeY) / BlockSizeY;
				uint32 NumLayers = FMath::Max<uint32>(1,ArraySize);
				
				glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

				if(bArrayTexture )
				{
					if(bCubeTexture)
					{
						check(FOpenGL::SupportsTexture3D());
						FOpenGL::TexSubImage3D(
							/*Target=*/ Target,
							/*Level=*/ MipIndex,
							/* XOffset */ 0,
							/* YOffset */ 0,
							/* ZOffset */ 0,
							/*SizeX=*/ FMath::Max<uint32>(1,(SizeX >> MipIndex)),
							/*SizeY=*/ FMath::Max<uint32>(1,(SizeY >> MipIndex)),
							/*SizeZ=*/ ArraySize,
							/*Format=*/ GLFormat.Format,
							/*Type=*/ GLFormat.Type,
							/*Data=*/ &Data[MipOffset]
							);
					}
					else
					{
						// @todo: refactor 2d texture arrays here?
						check(!bCubeTexture);
					}
					
					MipOffset += NumBlocksX * NumBlocksY * NumLayers * GPixelFormats[Format].BlockBytes;
				}
				else
				{
					GLenum FirstTarget = bCubeTexture ? GL_TEXTURE_CUBE_MAP_POSITIVE_X : Target;
					uint32 NumTargets = bCubeTexture ? 6 : 1;

					for(uint32 TargetIndex = 0; TargetIndex < NumTargets; TargetIndex++)
					{
						glTexSubImage2D(
							/*Target=*/ FirstTarget + TargetIndex,
							/*Level=*/ MipIndex,
							/*XOffset*/ 0,
							/*YOffset*/ 0,
							/*SizeX=*/ FMath::Max<uint32>(1,(SizeX >> MipIndex)),
							/*SizeY=*/ FMath::Max<uint32>(1,(SizeY >> MipIndex)),
							/*Format=*/ GLFormat.Format,
							/*Type=*/ GLFormat.Type,
							/*Data=*/ &Data[MipOffset]
							);
						
						MipOffset += NumBlocksX * NumBlocksY * NumLayers * GPixelFormats[Format].BlockBytes;
					}
				}

				glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
			}

			BulkData->Discard();
		}
	}
	else if (TileMemDepth)//CBR Hint 不使用TileMemDepth
	{
#if PLATFORM_ANDROID && !PLATFORM_LUMINGL4		
		Target = GL_RENDERBUFFER;
		glGenRenderbuffers(1, &TextureID);
		glBindRenderbuffer(GL_RENDERBUFFER, TextureID);
		glRenderbufferStorageMultisampleEXT(GL_RENDERBUFFER, NumSamplesTileMem, GL_DEPTH24_STENCIL8, SizeX, SizeY);
		VERIFY_GL(glRenderbufferStorageMultisampleEXT);
		glBindRenderbuffer(GL_RENDERBUFFER, 0);
#endif
	}//CBR Code
	else if (bDepth) {
		check(FOpenGL::SupportsMultisampledTextures());//需要返回true
		check(BulkData == NULL);

		// Try to create an immutable texture and fallback if it fails
		if (!FOpenGL::TexStorage2DMultisample(Target, NumSamples, GL_DEPTH24_STENCIL8, SizeX, SizeY, true))
		{
			FOpenGL::TexImage2DMultisample(
				Target,
				NumSamples,
				GL_DEPTH24_STENCIL8,
				SizeX,
				SizeY,
				true
				);
		}
	}//
	else
	{
		check( FOpenGL::SupportsMultisampledTextures() );
		check( BulkData == NULL);

		// Try to create an immutable texture and fallback if it fails
		if (!FOpenGL::TexStorage2DMultisample( Target, NumSamples, GLFormat.InternalFormat[bSRGB], SizeX, SizeY, true))
		{
			FOpenGL::TexImage2DMultisample(
				Target,
				NumSamples,
				GLFormat.InternalFormat[bSRGB],
				SizeX,
				SizeY,
				true
				);
		}
	}
	
	// Determine the attachment point for the texture.	
	GLenum Attachment = GL_NONE;
	if((Flags & TexCreate_RenderTargetable) || (Flags & TexCreate_CPUReadback))
	{
		Attachment = GL_COLOR_ATTACHMENT0;
	}
	else if(Flags & TexCreate_DepthStencilTargetable)
	{
		Attachment = (Format == PF_DepthStencil) ? GL_DEPTH_STENCIL_ATTACHMENT : GL_DEPTH_ATTACHMENT;
	}
	else if(Flags & TexCreate_ResolveTargetable)
	{
		Attachment = (Format == PF_DepthStencil)
						? GL_DEPTH_STENCIL_ATTACHMENT
						: ((Format == PF_ShadowDepth || Format == PF_D24)
							? GL_DEPTH_ATTACHMENT
							: GL_COLOR_ATTACHMENT0);
	}

	switch(Attachment)
	{
		case GL_COLOR_ATTACHMENT0:
			check(GMaxOpenGLColorSamples>=(GLint)NumSamples);
			break;
		case GL_DEPTH_ATTACHMENT:
		case GL_DEPTH_STENCIL_ATTACHMENT:
			check(GMaxOpenGLDepthSamples>=(GLint)NumSamples);
			break;
		default:
			break;
	}
	// @todo: If integer pixel format
	//check(GMaxOpenGLIntegerSamples>=NumSamples);

	if (bCubeTexture)
	{
		//	FOpenGLTextureCube* TextureCube = new FOpenGLTextureCube(this, TextureID, Target, Attachment, SizeX, SizeY, 0, NumMips, 1, 1, ArraySize, (EPixelFormat)Format, true, bAllocatedStorage, Flags, InClearValue);
		FOpenGLTextureCube* TextureCube = (FOpenGLTextureCube*)Texture;
		TextureCube->SetResource(TextureID);
		TextureCube->Target = Target;
		TextureCube->Attachment = Attachment;
		TextureCube->SetAllocatedStorage(bAllocatedStorage);
	}
	else
	{
		FOpenGLTexture2D* Texture2D = (FOpenGLTexture2D*)Texture;
		Texture2D->SetResource(TextureID);
		Texture2D->Target = Target;
		Texture2D->Attachment = Attachment;
		Texture2D->SetAllocatedStorage(bAllocatedStorage);
	}

	OpenGLTextureAllocated(Texture, Flags);
	// No need to restore texture stage; leave it like this,
	// and the next draw will take care of cleaning it up; or
	// next operation that needs the stage will switch something else in on it.
}

template<typename RHIResourceType>
void TOpenGLTexture<RHIResourceType>::Resolve(uint32 MipIndex,uint32 ArrayIndex)
{
	VERIFY_GL_SCOPE();
	
#if UE_BUILD_DEBUG
	if((FOpenGLTexture2D*)this->GetTexture2D())
	{
		check( ((FOpenGLTexture2D*)this->GetTexture2D())->GetNumSamples() == 1 );
	}
#endif
	
	// Calculate the dimensions of the mip-map.
	EPixelFormat PixelFormat = this->GetFormat();
	const uint32 BlockSizeX = GPixelFormats[PixelFormat].BlockSizeX;
	const uint32 BlockSizeY = GPixelFormats[PixelFormat].BlockSizeY;
	const uint32 BlockBytes = GPixelFormats[PixelFormat].BlockBytes;
	const uint32 MipSizeX = FMath::Max(this->GetSizeX() >> MipIndex,BlockSizeX);
	const uint32 MipSizeY = FMath::Max(this->GetSizeY() >> MipIndex,BlockSizeY);
	uint32 NumBlocksX = (MipSizeX + BlockSizeX - 1) / BlockSizeX;
	uint32 NumBlocksY = (MipSizeY + BlockSizeY - 1) / BlockSizeY;
	const uint32 MipBytes = NumBlocksX * NumBlocksY * BlockBytes;
	
	const int32 BufferIndex = MipIndex * (bCubemap ? 6 : 1) * this->GetEffectiveSizeZ() + ArrayIndex;
	
	// Standard path with a PBO mirroring ever slice of a texture to allow multiple simulataneous maps
	if (!IsValidRef(PixelBuffers[BufferIndex]))
	{
		PixelBuffers[BufferIndex] = new FOpenGLPixelBuffer(0, MipBytes, BUF_Dynamic);
	}
	
	TRefCountPtr<FOpenGLPixelBuffer> PixelBuffer = PixelBuffers[BufferIndex];
	check(PixelBuffer->GetSize() == MipBytes);
	check(!PixelBuffer->IsLocked());
	
	// Transfer data from texture to pixel buffer.
	// This may be further optimized by caching information if surface content was changed since last lock.
	
	const FOpenGLTextureFormat& GLFormat = GOpenGLTextureFormats[PixelFormat];
	const bool bSRGB = (this->GetFlags() & TexCreate_SRGB) != 0;
	
	// Use a texture stage that's not likely to be used for draws, to avoid waiting
	FOpenGLContextState& ContextState = OpenGLRHI->GetContextStateForCurrentContext();

	OpenGLRHI->CachedSetupTextureStage(ContextState, FOpenGL::GetMaxCombinedTextureImageUnits() - 1, Target, this->GetResource(), -1, this->GetNumMips());
	
	glBindBuffer( GL_PIXEL_PACK_BUFFER, PixelBuffer->Resource );

	{
		if( this->GetSizeZ() )
		{
			// apparently it's not possible to retrieve compressed image from GL_TEXTURE_2D_ARRAY in OpenGL for compressed images
			// and for uncompressed ones it's not possible to specify the image index
			check(0);
		}
		else
		{
			if (GLFormat.bCompressed)
			{
				FOpenGL::GetCompressedTexImage(
											   bCubemap ? GL_TEXTURE_CUBE_MAP_POSITIVE_X + ArrayIndex : Target,
											   MipIndex,
											   0);	// offset into PBO
			}
			else
			{
				glPixelStorei(GL_PACK_ALIGNMENT, 1);
				FOpenGL::GetTexImage(
									 bCubemap ? GL_TEXTURE_CUBE_MAP_POSITIVE_X + ArrayIndex : Target,
									 MipIndex,
									 GLFormat.Format,
									 GLFormat.Type,
									 0);	// offset into PBO
				glPixelStorei(GL_PACK_ALIGNMENT, 4);
			}
		}
	}
	
	glBindBuffer( GL_PIXEL_PACK_BUFFER, 0 );
	
	// No need to restore texture stage; leave it like this,
	// and the next draw will take care of cleaning it up; or
	// next operation that needs the stage will switch something else in on it.
}

template<typename RHIResourceType>
uint32 TOpenGLTexture<RHIResourceType>::GetLockSize(uint32 InMipIndex, uint32 ArrayIndex, EResourceLockMode LockMode, uint32& DestStride)
{
	// Calculate the dimensions of the mip-map.
	EPixelFormat PixelFormat = this->GetFormat();
	const uint32 BlockSizeX = GPixelFormats[PixelFormat].BlockSizeX;
	const uint32 BlockSizeY = GPixelFormats[PixelFormat].BlockSizeY;
	const uint32 BlockBytes = GPixelFormats[PixelFormat].BlockBytes;
	const uint32 MipSizeX = FMath::Max(this->GetSizeX() >> InMipIndex, BlockSizeX);
	const uint32 MipSizeY = FMath::Max(this->GetSizeY() >> InMipIndex, BlockSizeY);
	uint32 NumBlocksX = (MipSizeX + BlockSizeX - 1) / BlockSizeX;
	uint32 NumBlocksY = (MipSizeY + BlockSizeY - 1) / BlockSizeY;
	const uint32 MipBytes = NumBlocksX * NumBlocksY * BlockBytes;
	DestStride = NumBlocksX * BlockBytes;
	return MipBytes;
}


template<typename RHIResourceType>
void* TOpenGLTexture<RHIResourceType>::Lock(uint32 InMipIndex,uint32 ArrayIndex,EResourceLockMode LockMode,uint32& DestStride)
{
	VERIFY_GL_SCOPE();

#if UE_BUILD_DEBUG
	if((FOpenGLTexture2D*)this->GetTexture2D())
	{
		check( ((FOpenGLTexture2D*)this->GetTexture2D())->GetNumSamples() == 1 );
	}
#endif

	SCOPE_CYCLE_COUNTER(STAT_OpenGLLockTextureTime);
	
	const uint32 MipBytes = GetLockSize(InMipIndex, ArrayIndex, LockMode, DestStride);

	check(!IsEvicted() || ArrayIndex==0);
	void* result = NULL;

	const int32 BufferIndex = InMipIndex * (bCubemap ? 6 : 1) * this->GetEffectiveSizeZ() + ArrayIndex;
	EPixelFormat PixelFormat = this->GetFormat();

	// Should we use client-storage to improve update time on platforms that require it
	const FOpenGLTextureFormat& GLFormat = GOpenGLTextureFormats[PixelFormat];
	if (IsEvicted())
	{
		check(ArrayIndex == 0);
		// check there's nothing already here?
		ensure(InMipIndex >= (uint32)EvictionParamsPtr->MipImageData.Num() || EvictionParamsPtr->MipImageData[InMipIndex].Num() == 0);
		EvictionParamsPtr->SetMipData(InMipIndex, 0, MipBytes);
		return EvictionParamsPtr->MipImageData[InMipIndex].GetData();
	}
	else
	{
		// Standard path with a PBO mirroring ever slice of a texture to allow multiple simulataneous maps
		bool bBufferExists = true;
		if (!IsValidRef(PixelBuffers[BufferIndex]))
		{
			bBufferExists = false;
			PixelBuffers[BufferIndex] = new FOpenGLPixelBuffer(0, MipBytes, BUF_Dynamic);
		}

		TRefCountPtr<FOpenGLPixelBuffer> PixelBuffer = PixelBuffers[BufferIndex];
		check(PixelBuffer->GetSize() == MipBytes);
		check(!PixelBuffer->IsLocked());
		
		// If the buffer already exists & the flags are such that the texture cannot be rendered to & is CPU accessible then we can skip the internal resolve for read locks. This makes HZB occlusion faster.
		const bool bCPUTexResolved = bBufferExists && (this->GetFlags() & TexCreate_CPUReadback) && !(this->GetFlags() & (TexCreate_RenderTargetable|TexCreate_DepthStencilTargetable));

		if (LockMode != RLM_WriteOnly && !bCPUTexResolved)
		{
			Resolve(InMipIndex, ArrayIndex);
		}

		result = PixelBuffer->Lock(0, PixelBuffer->GetSize(), LockMode == RLM_ReadOnly, LockMode != RLM_ReadOnly);
	}
	
	return result;
}

// Copied from OpenGLDebugFrameDump.
inline uint32 HalfFloatToFloatInteger(uint16 HalfFloat)
{
	uint32 Sign = (HalfFloat >> 15) & 0x00000001;
	uint32 Exponent = (HalfFloat >> 10) & 0x0000001f;
	uint32 Mantiss = HalfFloat & 0x000003ff;

	if (Exponent == 0)
	{
		if (Mantiss == 0) // Plus or minus zero
		{
			return Sign << 31;
		}
		else // Denormalized number -- renormalize it
		{
			while ((Mantiss & 0x00000400) == 0)
			{
				Mantiss <<= 1;
				Exponent -= 1;
			}

			Exponent += 1;
			Mantiss &= ~0x00000400;
		}
	}
	else if (Exponent == 31)
	{
		if (Mantiss == 0) // Inf
			return (Sign << 31) | 0x7f800000;
		else // NaN
			return (Sign << 31) | 0x7f800000 | (Mantiss << 13);
	}

	Exponent = Exponent + (127 - 15);
	Mantiss = Mantiss << 13;

	return (Sign << 31) | (Exponent << 23) | Mantiss;
}

inline float HalfFloatToFloat(uint16 HalfFloat)
{
	union
	{
		float F;
		uint32 I;
	} Convert;

	Convert.I = HalfFloatToFloatInteger(HalfFloat);
	return Convert.F;
}

template<typename RHIResourceType>
void TOpenGLTexture<RHIResourceType>::Unlock(uint32 MipIndex,uint32 ArrayIndex)
{
	VERIFY_GL_SCOPE();
	SCOPE_CYCLE_COUNTER(STAT_OpenGLUnlockTextureTime);

	if (IsEvicted())
	{
		// evicted textures didn't actually perform a lock, so we can bail out early
		check(ArrayIndex == 0);
		// check the space was allocated
		ensure(MipIndex < (uint32)EvictionParamsPtr->MipImageData.Num() && EvictionParamsPtr->MipImageData[MipIndex].Num());
		return;
	}

	const int32 BufferIndex = MipIndex * (bCubemap ? 6 : 1) * this->GetEffectiveSizeZ() + ArrayIndex;
	const FOpenGLTextureFormat& GLFormat = GOpenGLTextureFormats[this->GetFormat()];
	const bool bSRGB = (this->GetFlags() & TexCreate_SRGB) != 0;
	TRefCountPtr<FOpenGLPixelBuffer> PixelBuffer = PixelBuffers[BufferIndex];

	check(IsValidRef(PixelBuffer));
	
#if PLATFORM_ANDROID && !PLATFORM_LUMINGL4
	// check for FloatRGBA to RGBA8 conversion needed
	if (this->GetFormat() == PF_FloatRGBA && GLFormat.Type == GL_UNSIGNED_BYTE)
	{
		UE_LOG(LogRHI, Warning, TEXT("Converting texture from PF_FloatRGBA to RGBA8!  Only supported for limited cases of 0.0 to 1.0 values (clamped)"));

		// Code path for non-PBO: and always uncompressed!
		// Volume/array textures are currently only supported if PixelBufferObjects are also supported.
		check(this->GetSizeZ() == 0);

		// Use a texture stage that's not likely to be used for draws, to avoid waiting
		FOpenGLContextState& ContextState = OpenGLRHI->GetContextStateForCurrentContext();
		OpenGLRHI->CachedSetupTextureStage(ContextState, FOpenGL::GetMaxCombinedTextureImageUnits() - 1, Target, GetResource(), -1, this->GetNumMips());

		CachedBindPixelUnpackBuffer(0);

		// get the source data and size
		uint16* floatData = (uint16*)PixelBuffer->GetLockedBuffer();
		int32 texWidth = FMath::Max<uint32>(1, (this->GetSizeX() >> MipIndex));
		int32 texHeight = FMath::Max<uint32>(1, (this->GetSizeY() >> MipIndex));

		// always RGBA8 so 4 bytes / pixel
		int nValues = texWidth * texHeight * 4;
		uint8* rgbaData = (uint8*)FMemory::Malloc(nValues);

		// convert to GL_BYTE (saturate)
		uint8* outPtr = rgbaData;
		while (nValues--)
		{
			int32 pixelValue = (int32)(HalfFloatToFloat(*floatData++) * 255.0f);
			*outPtr++ = (uint8)(pixelValue < 0 ? 0 : (pixelValue < 256 ? pixelValue : 255));
		}

		// All construction paths should have called TexStorage2D or TexImage2D. So we will
		// always call TexSubImage2D.
		check(GetAllocatedStorageForMip(MipIndex, ArrayIndex) == true);
		glTexSubImage2D(
			bCubemap ? GL_TEXTURE_CUBE_MAP_POSITIVE_X + ArrayIndex : Target,
			MipIndex,
			0,
			0,
			texWidth,
			texHeight,
			GLFormat.Format,
			GLFormat.Type,
			rgbaData);

		// free temporary conversion buffer
		FMemory::Free(rgbaData);

		// Unlock "PixelBuffer" and free the temp memory after the texture upload.
		PixelBuffer->Unlock();

		// No need to restore texture stage; leave it like this,
		// and the next draw will take care of cleaning it up; or
		// next operation that needs the stage will switch something else in on it.

		CachedBindPixelUnpackBuffer(0);

		return;
	}
#endif

	// Code path for PBO per slice
	check(IsValidRef(PixelBuffers[BufferIndex]));
			
	PixelBuffer->Unlock();

	// Modify permission?
	if (!PixelBuffer->IsLockReadOnly())
	{
		// Use a texture stage that's not likely to be used for draws, to avoid waiting
		FOpenGLContextState& ContextState = OpenGLRHI->GetContextStateForCurrentContext();
		OpenGLRHI->CachedSetupTextureStage(ContextState, FOpenGL::GetMaxCombinedTextureImageUnits() - 1, Target, GetResource(), -1, this->GetNumMips());

		if (this->GetSizeZ())
		{
			// texture 2D array
			if (GLFormat.bCompressed)
			{
				FOpenGL::CompressedTexSubImage3D(
					Target,
					MipIndex,
					0,
					0,
					ArrayIndex,
					FMath::Max<uint32>(1,(this->GetSizeX() >> MipIndex)),
					FMath::Max<uint32>(1,(this->GetSizeY() >> MipIndex)),
					1,
					GLFormat.InternalFormat[bSRGB],
					PixelBuffer->GetSize(),
					0);
			}
			else
			{
				glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
				check( FOpenGL::SupportsTexture3D() );
				FOpenGL::TexSubImage3D(
					Target,
					MipIndex,
					0,
					0,
					ArrayIndex,
					FMath::Max<uint32>(1,(this->GetSizeX() >> MipIndex)),
					FMath::Max<uint32>(1,(this->GetSizeY() >> MipIndex)),
					1,
					GLFormat.Format,
					GLFormat.Type,
					0);	// offset into PBO
				glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
			}
		}
		else
		{
			if (GLFormat.bCompressed)
			{
				if (GetAllocatedStorageForMip(MipIndex,ArrayIndex))
				{
					glCompressedTexSubImage2D(
						bCubemap ? GL_TEXTURE_CUBE_MAP_POSITIVE_X + ArrayIndex : Target,
						MipIndex,
						0,
						0,
						FMath::Max<uint32>(1,(this->GetSizeX() >> MipIndex)),
						FMath::Max<uint32>(1,(this->GetSizeY() >> MipIndex)),
						GLFormat.InternalFormat[bSRGB],
						PixelBuffer->GetSize(),
						0);	// offset into PBO
				}
				else
				{
					glCompressedTexImage2D(
						bCubemap ? GL_TEXTURE_CUBE_MAP_POSITIVE_X + ArrayIndex : Target,
						MipIndex,
						GLFormat.InternalFormat[bSRGB],
						FMath::Max<uint32>(1,(this->GetSizeX() >> MipIndex)),
						FMath::Max<uint32>(1,(this->GetSizeY() >> MipIndex)),
						0,
						PixelBuffer->GetSize(),
						0);	// offset into PBO
					SetAllocatedStorageForMip(MipIndex,ArrayIndex);
				}
			}
			else
			{
				// All construction paths should have called TexStorage2D or TexImage2D. So we will
				// always call TexSubImage2D.
				check(GetAllocatedStorageForMip(MipIndex,ArrayIndex) == true);
				glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
				glTexSubImage2D(
					bCubemap ? GL_TEXTURE_CUBE_MAP_POSITIVE_X + ArrayIndex : Target,
					MipIndex,
					0,
					0,
					FMath::Max<uint32>(1,(this->GetSizeX() >> MipIndex)),
					FMath::Max<uint32>(1,(this->GetSizeY() >> MipIndex)),
					GLFormat.Format,
					GLFormat.Type,
					0);	// offset into PBO
				glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
			}
		}
	}

	//need to free PBO if we aren't keeping shadow copies
	PixelBuffers[BufferIndex] = NULL;

	// No need to restore texture stage; leave it like this,
	// and the next draw will take care of cleaning it up; or
	// next operation that needs the stage will switch something else in on it.

	CachedBindPixelUnpackBuffer(0);
}


uint32 GTotalEvictedMipMemStored = 0;
uint32 GTotalEvictedMipMemDuplicated = 0;
uint32 GTotalMipStoredCount = 0;
uint32 GTotalMipRestores = 0;

extern uint32 GTotalEvictedMipMemStored;
extern uint32 GTotalTexStorageSkipped;

float GMaxRestoreTime = 0.0f;
float GAvgRestoreTime = 0.0f;
uint32 GAvgRestoreCount = 0;

template<typename RHIResourceType>
void TOpenGLTexture<RHIResourceType>::RestoreEvictedGLResource(bool bAttemptToRetainMips)
{
//	double StartTime = FPlatformTime::Seconds();

	QUICK_SCOPE_CYCLE_COUNTER(STAT_OpenGLRestoreEvictedTextureTime);

	check(!EvictionParamsPtr->bHasRestored);
	EvictionParamsPtr->bHasRestored = true;

	const FClearValueBinding ClearBinding = this->GetClearBinding();
	OpenGLRHI->InitializeGLTextureInternal(GetRawResourceName(), this, this->GetSizeX(), this->GetSizeY(), bCubemap, false, false, this->GetFormat(), this->GetNumMips(), this->GetNumSamples(), 0, this->GetFlags(), ClearBinding, nullptr);

	EPixelFormat PixelFormat = this->GetFormat();
	const FOpenGLTextureFormat& GLFormat = GOpenGLTextureFormats[PixelFormat];
	const bool bSRGB = (this->GetFlags() & TexCreate_SRGB) != 0;
	checkf(EvictionParamsPtr->MipImageData.Num() == this->GetNumMips(), TEXT("EvictionParamsPtr->MipImageData.Num() =%d, this->GetNumMips() = %d"), EvictionParamsPtr->MipImageData.Num(), this->GetNumMips());
	
	for (int i = EvictionParamsPtr->MipImageData.Num() - 1; i >= 0; i--)
	{
		auto& MipMem = EvictionParamsPtr->MipImageData[i];
		if(MipMem.Num())
		{
			uint32 DestStride;
			check(MipMem.Num() == GetLockSize(i, 0, EResourceLockMode::RLM_WriteOnly, DestStride));
			void* pDest = Lock(i, 0, EResourceLockMode::RLM_WriteOnly, DestStride);
			check(DestStride)
			FMemory::Memcpy(pDest, MipMem.GetData(), MipMem.Num());
			Unlock(i, 0);
		}
	}

	// Use the resident streaming mips if our cvar is -1.
	uint32 DeferTextureCreationKeepLowerMipCount = (uint32)(GOGLDeferTextureCreationKeepLowerMipCount >= 0 ? GOGLDeferTextureCreationKeepLowerMipCount : UTexture::GetStaticMinTextureResidentMipCount());

	uint32 RetainMips = bAttemptToRetainMips && (this->GetFlags() & TexCreate_Streamable) && this->GetNumMips() > 1 && !this->IsAliased() ? DeferTextureCreationKeepLowerMipCount : 0;

	if (CanBeEvicted())
	{
		if (FTextureEvictionLRU::Get().Add(this) == false)
		{
			// could not store this in the LRU. Deleting all backup mips, as this texture will never be evicted.
			RetainMips = 0;
		}
	}

	// keep the mips for streamable textures
	EvictionParamsPtr->ReleaseMipData(RetainMips);
#if GLDEBUG_LABELS_ENABLED
	FAnsiCharArray& TextureDebugName = EvictionParamsPtr->GetDebugLabelName();
	if(TextureDebugName.Num())
	{
		FOpenGL::LabelObject(GL_TEXTURE, this->GetRawResourceName(), TextureDebugName.GetData());
		if (RetainMips == 0)
		{
			TextureDebugName.Empty();
		}
	}
#endif

	GTotalEvictedMipMemDuplicated += EvictionParamsPtr->GetTotalAllocated();
// 	float ThisTime = (float)(FPlatformTime::Seconds() - StartTime);
// 	GAvgRestoreCount++;
// 	GMaxRestoreTime = FMath::Max(GMaxRestoreTime, ThisTime);
// 	GAvgRestoreTime += ThisTime;
}

template<typename RHIResourceType>
void TOpenGLTexture<RHIResourceType>::TryEvictGLResource()
{
	VERIFY_GL_SCOPE();
	if (CanCreateAsEvicted() && EvictionParamsPtr->bHasRestored)
	{

		if (CanBeEvicted())
		{
			DeleteGLResource();

			// create a new texture id.
			EvictionParamsPtr->bHasRestored = false;
			const FClearValueBinding ClearBinding = this->GetClearBinding();
			// recreate the GL tex resource name (but not allocate the memory)
			OpenGLRHI->InitializeGLTexture(this, this->GetSizeX(), this->GetSizeY(), bCubemap, false, false, this->GetFormat(), this->GetNumMips(), this->GetNumSamples(), 0, this->GetFlags(), ClearBinding, nullptr);
			GTotalEvictedMipMemDuplicated -= EvictionParamsPtr->GetTotalAllocated();
		}
	}
}

static bool CanDeferTextureCreation()
{
	bool bCanDeferTextureCreation = CVarDeferTextureCreation.GetValueOnAnyThread() != 0;
#if PLATFORM_ANDROID
	static bool bDeferTextureCreationConfigRulesChecked = false;
	static TOptional<bool> bConfigRulesCanDeferTextureCreation;
	if (!bDeferTextureCreationConfigRulesChecked)
	{
		const FString* ConfigRulesDeferOpenGLTextureCreationStr = FAndroidMisc::GetConfigRulesVariable(TEXT("DeferOpenGLTextureCreation"));
		if (ConfigRulesDeferOpenGLTextureCreationStr)
		{
			bConfigRulesCanDeferTextureCreation = ConfigRulesDeferOpenGLTextureCreationStr->Equals("true", ESearchCase::IgnoreCase);
			UE_LOG(LogRHI, Log, TEXT("OpenGL deferred texture creation, set by config rules: %d"), (int)bConfigRulesCanDeferTextureCreation.GetValue());
		}
		else
		{
			UE_LOG(LogRHI, Log, TEXT("OpenGL deferred texture creation, no config rule set: %d"), (int)bCanDeferTextureCreation);
		}
		bDeferTextureCreationConfigRulesChecked = true;
	}

	if (bConfigRulesCanDeferTextureCreation.IsSet())
	{
		bCanDeferTextureCreation = bConfigRulesCanDeferTextureCreation.GetValue();
	}
#endif
	return bCanDeferTextureCreation;
}

template<typename RHIResourceType>
bool TOpenGLTexture<RHIResourceType>::CanCreateAsEvicted()
{
	// can run on RT.
	bool bRet =
		CanDeferTextureCreation()
		&& FOpenGL::SupportsCopyImage()
		&& this->GetFlags() // ignore TexCreate_None
		&& (CVarDeferTextureCreationExcludeMask.GetValueOnAnyThread() & this->GetFlags())==0  // Anything outside of these flags cannot be evicted.
		&& Target == GL_TEXTURE_2D
		&& this->GetTexture2D(); // 2d only.

	if (GOGLTextureEvictLogging)
	{
		UE_CLOG(!bRet, LogRHI, Warning, TEXT("CanDeferTextureCreation:%d, SupportsCopyImage:%d, Flags:%x Flags&Mask:%d, Target:%x"), CanDeferTextureCreation(), FOpenGL::SupportsCopyImage(), this->GetFlags(), (CVarDeferTextureCreationExcludeMask.GetValueOnAnyThread() & this->GetFlags()), Target);
	}

	return bRet;
}

template<typename RHIResourceType>
bool TOpenGLTexture<RHIResourceType>::CanBeEvicted()
{
	VERIFY_GL_SCOPE();
	checkf(!CanCreateAsEvicted() || EvictionParamsPtr.IsValid(), TEXT("%p, CanCreateAsEvicted() %d, EvictionParamsPtr.IsValid() %d"), this, CanCreateAsEvicted(),EvictionParamsPtr.IsValid());

	// if we're aliased check that there's no eviction data.
	check(!CanCreateAsEvicted() || !this->IsAliased() || (EvictionParamsPtr->MipImageData.Num() == 0 && EvictionParamsPtr->MipImageData.Num() != this->GetNumMips()));

	// cant evict if we're aliased, or there are mips are not backed by stored data.
	bool bRet = CanCreateAsEvicted() && EvictionParamsPtr->MipImageData.Num() == this->GetNumMips() && EvictionParamsPtr->AreAllMipsPresent();

	return bRet;
}

template<typename RHIResourceType>
void TOpenGLTexture<RHIResourceType>::CloneViaCopyImage( TOpenGLTexture<RHIResourceType>* Src, uint32 InNumMips, int32 SrcOffset, int32 DstOffset)
{
	VERIFY_GL_SCOPE();

	check(FOpenGL::SupportsCopyImage());
	
	check(Src->CanCreateAsEvicted() == CanCreateAsEvicted());
	if (CanCreateAsEvicted())
	{
		// Copy all mips that are present.
		if (!(!Src->IsEvicted() || Src->EvictionParamsPtr->AreAllMipsPresent()))
		{
			UE_LOG(LogRHI, Warning, TEXT("IsEvicted %d, MipsPresent %d, InNumMips %d, SrcOffset %d, DstOffset %d"), Src->IsEvicted(), Src->EvictionParamsPtr->AreAllMipsPresent(), InNumMips, SrcOffset, DstOffset);
			int MessageCount = 0;
			for (const auto& MipData : Src->EvictionParamsPtr->MipImageData)
			{
				UE_LOG(LogRHI, Warning, TEXT("SrcMipData[%d].Num() == %d"), MessageCount++, MipData.Num());
			}	
		}
		check(!Src->IsEvicted() || Src->EvictionParamsPtr->AreAllMipsPresent() );
		EvictionParamsPtr->CloneMipData(*Src->EvictionParamsPtr, InNumMips, SrcOffset, DstOffset);

		// the dest texture can remain evicted if: the src was also evicted or has all of the resident mips available or the dest texture has all mips already evicted.
		if(IsEvicted() && (Src->IsEvicted() || Src->EvictionParamsPtr->AreAllMipsPresent() || EvictionParamsPtr->AreAllMipsPresent()))
		{
			return;
		}
	}

	for (uint32 ArrayIndex = 0; ArrayIndex < this->GetEffectiveSizeZ(); ArrayIndex++)
	{
		// use the Copy Image functionality to copy mip level by mip level
		for(uint32 MipIndex = 0;MipIndex < InNumMips;++MipIndex)
		{
			// Calculate the dimensions of the mip-map.
			const uint32 DstMipIndex = MipIndex + DstOffset;
			const uint32 SrcMipIndex = MipIndex + SrcOffset;
			const uint32 MipSizeX = FMath::Max(this->GetSizeX() >> DstMipIndex,uint32(1));
			const uint32 MipSizeY = FMath::Max(this->GetSizeY() >> DstMipIndex,uint32(1));
			
			if(FOpenGL::AmdWorkaround() && ((MipSizeX < 4) || (MipSizeY < 4))) break;

			// copy the texture data
			FOpenGL::CopyImageSubData(Src->GetResource(), Src->Target, SrcMipIndex, 0, 0, ArrayIndex,
				GetResource(), Target, DstMipIndex, 0, 0, ArrayIndex, MipSizeX, MipSizeY, 1);
		}
	}
	
}

template<typename RHIResourceType>
void TOpenGLTexture<RHIResourceType>::CloneViaPBO( TOpenGLTexture<RHIResourceType>* Src, uint32 InNumMips, int32 SrcOffset, int32 DstOffset)
{
	VERIFY_GL_SCOPE();
	
	// apparently it's not possible to retrieve compressed image from GL_TEXTURE_2D_ARRAY in OpenGL for compressed images
	// and for uncompressed ones it's not possible to specify the image index
	check(this->GetSizeZ() == 0);

	EPixelFormat PixelFormat = this->GetFormat();
	check(PixelFormat == Src->GetFormat());

	const FOpenGLTextureFormat& GLFormat = GOpenGLTextureFormats[PixelFormat];
	const bool bSRGB = (this->GetFlags() & TexCreate_SRGB) != 0;
	check(bSRGB == ((Src->GetFlags() & TexCreate_SRGB) != 0));
	
	const uint32 BlockSizeX = GPixelFormats[PixelFormat].BlockSizeX;
	const uint32 BlockSizeY = GPixelFormats[PixelFormat].BlockSizeY;
	const uint32 BlockBytes = GPixelFormats[PixelFormat].BlockBytes;
	
	FOpenGLContextState& ContextState = OpenGLRHI->GetContextStateForCurrentContext();
	
	for (uint32 ArrayIndex = 0; ArrayIndex < this->GetEffectiveSizeZ(); ArrayIndex++)
	{
		// use PBO functionality to copy mip level by mip level
		for(uint32 MipIndex = 0;MipIndex < InNumMips;++MipIndex)
		{
			// Actual mip levels
			const uint32 DstMipIndex = MipIndex + DstOffset;
			const uint32 SrcMipIndex = MipIndex + SrcOffset;
			
			// Calculate the dimensions of the mip-map.
			const uint32 MipSizeX = FMath::Max(this->GetSizeX() >> DstMipIndex,1u);
			const uint32 MipSizeY = FMath::Max(this->GetSizeY() >> DstMipIndex,1u);
			
			// Then the rounded PBO size required to capture this mip
			const uint32 DataSizeX = FMath::Max(MipSizeX,BlockSizeX);
			const uint32 DataSizeY = FMath::Max(MipSizeY,BlockSizeY);
			uint32 NumBlocksX = (DataSizeX + BlockSizeX - 1) / BlockSizeX;
			uint32 NumBlocksY = (DataSizeY + BlockSizeY - 1) / BlockSizeY;

			const uint32 MipBytes = NumBlocksX * NumBlocksY * BlockBytes;
			const int32 BufferIndex = DstMipIndex * (bCubemap ? 6 : 1) * this->GetEffectiveSizeZ() + ArrayIndex;
			const int32 SrcBufferIndex = SrcMipIndex * (Src->bCubemap ? 6 : 1) * Src->GetEffectiveSizeZ() + ArrayIndex;
			
			// Standard path with a PBO mirroring ever slice of a texture to allow multiple simulataneous maps
			if (!IsValidRef(PixelBuffers[BufferIndex]))
			{
				PixelBuffers[BufferIndex] = new FOpenGLPixelBuffer(0, MipBytes, BUF_Dynamic);
			}
			
			TRefCountPtr<FOpenGLPixelBuffer> PixelBuffer = PixelBuffers[BufferIndex];
			check(PixelBuffer->GetSize() == MipBytes);
			check(!PixelBuffer->IsLocked());
			
			// Transfer data from texture to pixel buffer.
			// This may be further optimized by caching information if surface content was changed since last lock.
			// Use a texture stage that's not likely to be used for draws, to avoid waiting
			OpenGLRHI->CachedSetupTextureStage(ContextState, FOpenGL::GetMaxCombinedTextureImageUnits() - 1, Src->Target, Src->GetResource(), -1, this->GetNumMips());
			
			glBindBuffer( GL_PIXEL_PACK_BUFFER, PixelBuffer->Resource );
			
			if (GLFormat.bCompressed)
			{
				FOpenGL::GetCompressedTexImage(Src->bCubemap ? GL_TEXTURE_CUBE_MAP_POSITIVE_X + ArrayIndex : Src->Target,
											   SrcMipIndex,
											   0);	// offset into PBO
			}
			else
			{
				glPixelStorei(GL_PACK_ALIGNMENT, 1);
				FOpenGL::GetTexImage(Src->bCubemap ? GL_TEXTURE_CUBE_MAP_POSITIVE_X + ArrayIndex : Src->Target,
									 SrcMipIndex,
									 GLFormat.Format,
									 GLFormat.Type,
									 0);	// offset into PBO
				glPixelStorei(GL_PACK_ALIGNMENT, 4);
			}
			
			// copy the texture data
			// Upload directly into Dst to avoid out-of-band synchronization caused by glMapBuffer!
			{
				CachedBindPixelUnpackBuffer( PixelBuffer->Resource );
				
				// Use a texture stage that's not likely to be used for draws, to avoid waiting
				OpenGLRHI->CachedSetupTextureStage(ContextState, FOpenGL::GetMaxCombinedTextureImageUnits() - 1, Target, GetResource(), -1, this->GetNumMips());
				
				if( this->GetSizeZ() )
				{
					// texture 2D array
					if (GLFormat.bCompressed)
					{
						FOpenGL::CompressedTexSubImage3D(Target,
														 DstMipIndex,
														 0,
														 0,
														 ArrayIndex,
														 MipSizeX,
														 MipSizeY,
														 1,
														 GLFormat.InternalFormat[bSRGB],
														 PixelBuffer->GetSize(),
														 0);
					}
					else
					{
						glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
						check( FOpenGL::SupportsTexture3D() );
						FOpenGL::TexSubImage3D(Target,
											   DstMipIndex,
											   0,
											   0,
											   ArrayIndex,
											   MipSizeX,
											   MipSizeY,
											   1,
											   GLFormat.Format,
											   GLFormat.Type,
											   0);	// offset into PBO
						glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
					}
				}
				else
				{
					if (GLFormat.bCompressed)
					{
						if (GetAllocatedStorageForMip(DstMipIndex,ArrayIndex))
						{
							glCompressedTexSubImage2D(bCubemap ? GL_TEXTURE_CUBE_MAP_POSITIVE_X + ArrayIndex : Target,
													  DstMipIndex,
													  0,
													  0,
													  MipSizeX,
													  MipSizeY,
													  GLFormat.InternalFormat[bSRGB],
													  PixelBuffer->GetSize(),
													  0);	// offset into PBO
						}
						else
						{
							glCompressedTexImage2D(bCubemap ? GL_TEXTURE_CUBE_MAP_POSITIVE_X + ArrayIndex : Target,
												   DstMipIndex,
												   GLFormat.InternalFormat[bSRGB],
												   MipSizeX,
												   MipSizeY,
												   0,
												   PixelBuffer->GetSize(),
												   0);	// offset into PBO
							SetAllocatedStorageForMip(DstMipIndex,ArrayIndex);
						}
					}
					else
					{
						// All construction paths should have called TexStorage2D or TexImage2D. So we will
						// always call TexSubImage2D.
						check(GetAllocatedStorageForMip(DstMipIndex,ArrayIndex) == true);
						glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
						glTexSubImage2D(bCubemap ? GL_TEXTURE_CUBE_MAP_POSITIVE_X + ArrayIndex : Target,
										DstMipIndex,
										0,
										0,
										MipSizeX,
										MipSizeY,
										GLFormat.Format,
										GLFormat.Type,
										0);	// offset into PBO
						glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
					}
				}
			}
			
			// need to free PBO if we aren't keeping shadow copies
			PixelBuffers[BufferIndex] = NULL;
			
			// No need to restore texture stage; leave it like this,
			// and the next draw will take care of cleaning it up; or
			// next operation that needs the stage will switch something else in on it.
		}
	}
	
	// Reset the buffer bindings on exit only
	glBindBuffer( GL_PIXEL_PACK_BUFFER, 0 );
	CachedBindPixelUnpackBuffer(0);
}

template class TOpenGLTexture<FOpenGLBaseTexture>;

/*-----------------------------------------------------------------------------
	2D texture support.
-----------------------------------------------------------------------------*/

/**
* Creates a 2D RHI texture resource
* @param SizeX - width of the texture to create
* @param SizeY - height of the texture to create
* @param Format - EPixelFormat texture format
* @param NumMips - number of mips to generate or 0 for full mip pyramid
* @param Flags - ETextureCreateFlags creation flags
*/
FTexture2DRHIRef FOpenGLDynamicRHI::RHICreateTexture2D(uint32 SizeX,uint32 SizeY,uint8 Format,uint32 NumMips,uint32 NumSamples,ETextureCreateFlags Flags, ERHIAccess InResourceState,FRHIResourceCreateInfo& Info)
{
	return (FRHITexture2D*)CreateOpenGLTexture(SizeX, SizeY, false, false, false, Format, NumMips, NumSamples, 1, Flags, Info.ClearValueBinding, Info.BulkData);
}

/**
* Creates a 2D RHI texture external resource
* @param SizeX - width of the texture to create
* @param SizeY - height of the texture to create
* @param Format - EPixelFormat texture format
* @param NumMips - number of mips to generate or 0 for full mip pyramid
* @param Flags - ETextureCreateFlags creation flags
*/
FTexture2DRHIRef FOpenGLDynamicRHI::RHICreateTextureExternal2D(uint32 SizeX, uint32 SizeY, uint8 Format, uint32 NumMips, uint32 NumSamples, ETextureCreateFlags Flags, ERHIAccess InResourceState, FRHIResourceCreateInfo& Info)
{
	return (FRHITexture2D*)CreateOpenGLTexture(SizeX, SizeY, false, false, true, Format, NumMips, NumSamples, 1, Flags, Info.ClearValueBinding, Info.BulkData);
}

FTexture2DRHIRef FOpenGLDynamicRHI::RHIAsyncCreateTexture2D(uint32 SizeX, uint32 SizeY, uint8 Format, uint32 NumMips, ETextureCreateFlags Flags, ERHIAccess InResourceState, void** InitialMipData, uint32 NumInitialMips)
{
	check(0);
	return FTexture2DRHIRef();
}

void FOpenGLDynamicRHI::RHICopySharedMips(FRHITexture2D* DestTexture2D, FRHITexture2D* SrcTexture2D)
{
	check(0);
}

FTexture2DArrayRHIRef FOpenGLDynamicRHI::RHICreateTexture2DArray(uint32 SizeX,uint32 SizeY,uint32 SizeZ,uint8 Format,uint32 NumMips,uint32 NumSamples,ETextureCreateFlags Flags, ERHIAccess InResourceState, FRHIResourceCreateInfo& Info)
{
	VERIFY_GL_SCOPE();

	SCOPE_CYCLE_COUNTER(STAT_OpenGLCreateTextureTime);

	check( FOpenGL::SupportsTexture3D() );

	if(NumMips == 0)
	{
		NumMips = FindMaxMipmapLevel(SizeX, SizeY);
	}

	GLuint TextureID = 0;
	FOpenGL::GenTextures(1, &TextureID);

	const GLenum Target = GL_TEXTURE_2D_ARRAY;

	// Use a texture stage that's not likely to be used for draws, to avoid waiting
	FOpenGLContextState& ContextState = GetContextStateForCurrentContext();
	CachedSetupTextureStage(ContextState, FOpenGL::GetMaxCombinedTextureImageUnits() - 1, Target, TextureID, 0, NumMips);

	glTexParameteri(Target, GL_TEXTURE_WRAP_S, GL_REPEAT);
	glTexParameteri(Target, GL_TEXTURE_WRAP_T, GL_REPEAT);
	glTexParameteri(Target, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(Target, GL_TEXTURE_MIN_FILTER, NumMips > 1 ? GL_NEAREST_MIPMAP_NEAREST : GL_NEAREST);
	if( FOpenGL::SupportsTextureFilterAnisotropic() )
	{
		glTexParameteri(Target, GL_TEXTURE_MAX_ANISOTROPY_EXT, 1);
	}
	glTexParameteri(Target, GL_TEXTURE_BASE_LEVEL, 0);
	glTexParameteri(Target, GL_TEXTURE_MAX_LEVEL, NumMips - 1);
	
	TextureMipLimits.Add(TextureID, TPair<GLenum, GLenum>(0, NumMips - 1));

	const bool bSRGB = (Flags&TexCreate_SRGB) != 0;
	const FOpenGLTextureFormat& GLFormat = GOpenGLTextureFormats[Format];
	const FPixelFormatInfo& FormatInfo = GPixelFormats[Format];
	if (GLFormat.InternalFormat[bSRGB] == GL_NONE)
	{
		UE_LOG(LogRHI, Fatal,TEXT("Texture format '%s' not supported."), FormatInfo.Name);
	}

	// Make sure PBO is disabled
	CachedBindPixelUnpackBuffer(ContextState, 0);

	uint8* Data = Info.BulkData ? (uint8*)Info.BulkData->GetResourceBulkData() : NULL;
	uint32 DataSize = Info.BulkData ? Info.BulkData->GetResourceBulkDataSize() : 0;
	uint32 MipOffset = 0;

	FOpenGL::TexStorage3D( Target, NumMips, GLFormat.SizedInternalFormat[bSRGB], SizeX, SizeY, SizeZ, GLFormat.Format, GLFormat.Type );

	if (Data)
	{
		for(uint32 MipIndex = 0; MipIndex < NumMips; MipIndex++)
		{
			const int32 MipSizeX = FMath::Max<int32>(1, (SizeX >> MipIndex));
			const int32 MipSizeY = FMath::Max<int32>(1, (SizeY >> MipIndex));

			const uint32 MipLinePitch = FMath::DivideAndRoundUp(MipSizeX, FormatInfo.BlockSizeX) * FormatInfo.BlockBytes;
			const uint32 MipSlicePitch = FMath::DivideAndRoundUp(MipSizeY, FormatInfo.BlockSizeY) * MipLinePitch;
			const uint32 MipSize = MipSlicePitch * SizeZ;

			if (MipOffset + MipSize > DataSize)
			{
				break; // Stop if the texture does not contain the mips.
			}
						
			if (GLFormat.bCompressed)
			{
				FOpenGL::CompressedTexSubImage3D(
					Target,
					MipIndex,
					0, 0, 0,
					MipSizeX, MipSizeY, SizeZ,
					GLFormat.InternalFormat[bSRGB],
					MipSize,
					&Data[MipOffset]);
			}
			else
			{
				glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
				FOpenGL::TexSubImage3D(
					Target,
					MipIndex,
					0, 0, 0,
					MipSizeX, MipSizeY, SizeZ,
					GLFormat.Format,
					GLFormat.Type,
					&Data[MipOffset]
					);
				glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
			}

			MipOffset+= MipSize;
		}

		Info.BulkData->Discard();
	}
	
	// Determine the attachment point for the texture.	
	GLenum Attachment = GL_NONE;
	if(Flags & TexCreate_RenderTargetable)
	{
		Attachment = GL_COLOR_ATTACHMENT0;
	}
	else if(Flags & TexCreate_DepthStencilTargetable)
	{
		Attachment = (Format == PF_DepthStencil) ? GL_DEPTH_STENCIL_ATTACHMENT : GL_DEPTH_ATTACHMENT;
	}
	else if(Flags & TexCreate_ResolveTargetable)
	{
		Attachment = (Format == PF_DepthStencil)
			? GL_DEPTH_STENCIL_ATTACHMENT
			: ((Format == PF_ShadowDepth || Format == PF_D24)
			? GL_DEPTH_ATTACHMENT
			: GL_COLOR_ATTACHMENT0);
	}

	FOpenGLTexture2DArray* Texture = new FOpenGLTexture2DArray(this,TextureID,Target,Attachment,SizeX,SizeY,SizeZ,NumMips,1, 1, SizeZ, (EPixelFormat)Format,false,true,Flags,Info.ClearValueBinding);
	OpenGLTextureAllocated( Texture, Flags );

	// No need to restore texture stage; leave it like this,
	// and the next draw will take care of cleaning it up; or
	// next operation that needs the stage will switch something else in on it.

	return Texture;
}

FTexture3DRHIRef FOpenGLDynamicRHI::RHICreateTexture3D(uint32 SizeX,uint32 SizeY,uint32 SizeZ,uint8 Format,uint32 NumMips,ETextureCreateFlags Flags, ERHIAccess InResourceState,FRHIResourceCreateInfo& CreateInfo)
{
	VERIFY_GL_SCOPE();

	SCOPE_CYCLE_COUNTER(STAT_OpenGLCreateTextureTime);

	check( FOpenGL::SupportsTexture3D() );

	if(NumMips == 0)
	{
		NumMips = FindMaxMipmapLevel(SizeX, SizeY, SizeZ);
	}

	GLuint TextureID = 0;
	FOpenGL::GenTextures(1, &TextureID);

	const GLenum Target = GL_TEXTURE_3D;

	// Use a texture stage that's not likely to be used for draws, to avoid waiting
	FOpenGLContextState& ContextState = GetContextStateForCurrentContext();
	CachedSetupTextureStage(ContextState, FOpenGL::GetMaxCombinedTextureImageUnits() - 1, Target, TextureID, 0, NumMips);

	glTexParameteri(Target, GL_TEXTURE_WRAP_S, GL_REPEAT);
	glTexParameteri(Target, GL_TEXTURE_WRAP_T, GL_REPEAT);
	glTexParameteri(Target, GL_TEXTURE_WRAP_R, GL_REPEAT);
	glTexParameteri(Target, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(Target, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_NEAREST);
	if( FOpenGL::SupportsTextureFilterAnisotropic() )
	{
		glTexParameteri(Target, GL_TEXTURE_MAX_ANISOTROPY_EXT, 1);
	}
	glTexParameteri(Target, GL_TEXTURE_BASE_LEVEL, 0);
	glTexParameteri(Target, GL_TEXTURE_MAX_LEVEL, NumMips - 1);
	
	TextureMipLimits.Add(TextureID, TPair<GLenum, GLenum>(0, NumMips - 1));

	const bool bSRGB = (Flags&TexCreate_SRGB) != 0;
	const FOpenGLTextureFormat& GLFormat = GOpenGLTextureFormats[Format];
	const FPixelFormatInfo& FormatInfo = GPixelFormats[Format];

	if (GLFormat.InternalFormat[bSRGB] == GL_NONE)
	{
		UE_LOG(LogRHI, Fatal,TEXT("Texture format '%s' not supported."), FormatInfo.Name);
	}

	// Make sure PBO is disabled
	CachedBindPixelUnpackBuffer(ContextState,0);

	uint8* Data = CreateInfo.BulkData ? (uint8*)CreateInfo.BulkData->GetResourceBulkData() : nullptr;
	uint32 DataSize = CreateInfo.BulkData ? CreateInfo.BulkData->GetResourceBulkDataSize() : 0;
	uint32 MipOffset = 0;

	FOpenGL::TexStorage3D( Target, NumMips, GLFormat.SizedInternalFormat[bSRGB], SizeX, SizeY, SizeZ, GLFormat.Format, GLFormat.Type );

	if (Data)
	{
		for (uint32 MipIndex = 0; MipIndex < NumMips; MipIndex++)
		{
			const int32 MipSizeX = FMath::Max<int32>(1, (SizeX >> MipIndex));
			const int32 MipSizeY = FMath::Max<int32>(1, (SizeY >> MipIndex));
			const int32 MipSizeZ = FMath::Max<int32>(1, (SizeZ >> MipIndex));

			const uint32 MipLinePitch = FMath::DivideAndRoundUp(MipSizeX, FormatInfo.BlockSizeX) * FormatInfo.BlockBytes;
			const uint32 MipSlicePitch = FMath::DivideAndRoundUp(MipSizeY, FormatInfo.BlockSizeY) * MipLinePitch;
			const uint32 MipSize = MipSlicePitch * MipSizeZ;

			if (MipOffset + MipSize > DataSize)
			{
				break; // Stop if the texture does not contain the mips.
			}

			if (GLFormat.bCompressed)
			{
				int32 RowLength = FMath::DivideAndRoundUp(MipSizeX, FormatInfo.BlockSizeX) * FormatInfo.BlockSizeX;
				int32 ImageHeight = FMath::DivideAndRoundUp(MipSizeY, FormatInfo.BlockSizeY) * FormatInfo.BlockSizeY;

				FOpenGL::CompressedTexSubImage3D(
					Target,
					MipIndex,
					0, 0, 0,
					MipSizeX, MipSizeY, MipSizeZ,
					GLFormat.InternalFormat[bSRGB],
					MipSize,
					Data + MipOffset);
			}
			else
			{
				glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
				FOpenGL::TexSubImage3D(
					/*Target=*/ Target,
					/*Level=*/ MipIndex,
					0, 0, 0, 
					MipSizeX, MipSizeY, MipSizeZ,
					/*Format=*/ GLFormat.Format,
					/*Type=*/ GLFormat.Type,
					/*Data=*/ Data + MipOffset);
				glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
			}

			MipOffset += MipSize;
		}

		CreateInfo.BulkData->Discard();
	}
	
	// Determine the attachment point for the texture.	
	GLenum Attachment = GL_NONE;
	if(Flags & TexCreate_RenderTargetable)
	{
		Attachment = GL_COLOR_ATTACHMENT0;
	}
	else if(Flags & TexCreate_DepthStencilTargetable)
	{
		Attachment = Format == PF_DepthStencil ? GL_DEPTH_STENCIL_ATTACHMENT : GL_DEPTH_ATTACHMENT;
	}
	else if(Flags & TexCreate_ResolveTargetable)
	{
		Attachment = (Format == PF_DepthStencil)
			? GL_DEPTH_STENCIL_ATTACHMENT
			: ((Format == PF_ShadowDepth || Format == PF_D24)
			? GL_DEPTH_ATTACHMENT
			: GL_COLOR_ATTACHMENT0);
	}

	FOpenGLTexture3D* Texture = new FOpenGLTexture3D(this,TextureID,Target,Attachment,SizeX,SizeY,SizeZ,NumMips,1,1,1, (EPixelFormat)Format,false,true,Flags,CreateInfo.ClearValueBinding);
	OpenGLTextureAllocated( Texture, Flags );

	// No need to restore texture stage; leave it like this,
	// and the next draw will take care of cleaning it up; or
	// next operation that needs the stage will switch something else in on it.

	return Texture;
}

void FOpenGLDynamicRHI::RHIGetResourceInfo(FRHITexture* Ref, FRHIResourceInfo& OutInfo)
{
}

FShaderResourceViewRHIRef FOpenGLDynamicRHI::RHICreateShaderResourceView(FRHITexture* Texture, const FRHITextureSRVCreateInfo& CreateInfo)
{
	const uint32 MipLevel = CreateInfo.MipLevel;
	const uint32 NumMipLevels = CreateInfo.NumMipLevels;
	EPixelFormat TextureBaseFormat = Texture->GetFormat();
	const uint8 Format = (CreateInfo.Format == PF_Unknown) ? TextureBaseFormat : CreateInfo.Format;

	FOpenGLShaderResourceViewProxy *ViewProxy = new FOpenGLShaderResourceViewProxy([this, Texture, MipLevel, NumMipLevels, Format](FRHIShaderResourceView* OwnerRHI) -> FOpenGLShaderResourceView*
	{
		if (FRHITexture2D* Texture2DRHI = Texture->GetTexture2D())
		{
			FOpenGLTexture2D* Texture2D = ResourceCast(Texture2DRHI);
			FOpenGLShaderResourceView *View = 0;

			if (FOpenGL::SupportsTextureView())
			{
				VERIFY_GL_SCOPE();

				GLuint Resource = 0;

				FOpenGL::GenTextures(1, &Resource);

				if (Format != PF_X24_G8)
				{
					// Choose original format when PF_Unknown is specified (as stated for FRHITextureSRVCreateInfo::Format)
					const FOpenGLTextureFormat& GLFormat = GOpenGLTextureFormats[Format == PF_Unknown ? Texture2D->GetFormat() : Format];
					const bool bSRGB = (Texture2D->GetFlags()&TexCreate_SRGB) != 0;

					FOpenGL::TextureView(Resource, Texture2D->Target, Texture2D->GetResource(), GLFormat.InternalFormat[bSRGB], MipLevel, NumMipLevels, 0, 1);
				}
				else
				{
					// PF_X24_G8 doesn't correspond to a real format under OpenGL
					// The solution is to create a view with the original format, and convert it to return the stencil index
					// To match component locations, texture swizzle needs to be setup too
					const FOpenGLTextureFormat& GLFormat = GOpenGLTextureFormats[Texture2D->GetFormat()];

					// create a second depth/stencil view
					FOpenGL::TextureView(Resource, Texture2D->Target, Texture2D->GetResource(), GLFormat.InternalFormat[0], MipLevel, NumMipLevels, 0, 1);

					// Use a texture stage that's not likely to be used for draws, to avoid waiting
					FOpenGLContextState& ContextState = GetContextStateForCurrentContext();
					CachedSetupTextureStage(ContextState, FOpenGL::GetMaxCombinedTextureImageUnits() - 1, Texture2D->Target, Resource, 0, NumMipLevels);

					//set the texture to return the stencil index, and then force the components to match D3D
					glTexParameteri(Texture2D->Target, GL_DEPTH_STENCIL_TEXTURE_MODE, GL_STENCIL_INDEX);
					glTexParameteri(Texture2D->Target, GL_TEXTURE_SWIZZLE_R, GL_ZERO);
					glTexParameteri(Texture2D->Target, GL_TEXTURE_SWIZZLE_G, GL_RED);
					glTexParameteri(Texture2D->Target, GL_TEXTURE_SWIZZLE_B, GL_ZERO);
					glTexParameteri(Texture2D->Target, GL_TEXTURE_SWIZZLE_A, GL_ZERO);
				}

				View = new FOpenGLShaderResourceView(this, Resource, Texture2D->Target, MipLevel, true);
			}
			else
			{
				uint32 const Target = Texture2D->Target;
				GLuint Resource = Texture2D->GetResource();

				FRHITexture2D* DepthStencilTex = nullptr;

				// For stencil sampling we have to use a separate single channel texture to blit stencil data into
#if PLATFORM_DESKTOP
				if (FOpenGL::GetFeatureLevel() >= ERHIFeatureLevel::SM5 && Format == PF_X24_G8)
				{
					check(NumMipLevels == 1 && MipLevel == 0);

					if (!Texture2D->SRVResource)
					{
						FOpenGL::GenTextures(1, &Texture2D->SRVResource);

						GLenum const InternalFormat = GL_R8UI;
						GLenum const ChannelFormat = GL_RED_INTEGER;
						uint32 const SizeX = Texture2D->GetSizeX();
						uint32 const SizeY = Texture2D->GetSizeY();
						GLenum const Type = GL_UNSIGNED_BYTE;
						uint32 const Flags = 0;

						FOpenGLContextState& ContextState = GetContextStateForCurrentContext();
						CachedSetupTextureStage(ContextState, FOpenGL::GetMaxCombinedTextureImageUnits() - 1, Target, Texture2D->SRVResource, MipLevel, NumMipLevels);

						if (!FOpenGL::TexStorage2D(Target, NumMipLevels, InternalFormat, SizeX, SizeY, ChannelFormat, Type, Flags))
						{
							glTexImage2D(Target, 0, InternalFormat, SizeX, SizeY, 0, ChannelFormat, Type, nullptr);
						}

						TArray<uint8> ZeroData;
						ZeroData.AddZeroed(SizeX * SizeY);

						glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
						glTexSubImage2D(
							Target,
							0,
							0,
							0,
							SizeX,
							SizeY,
							ChannelFormat,
							Type,
							ZeroData.GetData());
						glPixelStorei(GL_UNPACK_ALIGNMENT, 4);

						//set the texture to return the stencil index, and then force the components to match D3D
						glTexParameteri(Target, GL_TEXTURE_SWIZZLE_R, GL_ZERO);
						glTexParameteri(Target, GL_TEXTURE_SWIZZLE_G, GL_RED);
						glTexParameteri(Target, GL_TEXTURE_SWIZZLE_B, GL_ZERO);
						glTexParameteri(Target, GL_TEXTURE_SWIZZLE_A, GL_ZERO);
					}
					check(Texture2D->SRVResource);

					Resource = Texture2D->SRVResource;
					DepthStencilTex = Texture2DRHI;
				}
#endif

				View = new FOpenGLShaderResourceView(this, Resource, Target, MipLevel, false);
				View->Texture2D = DepthStencilTex;
			}
			return View;
		}
		else if (FRHITexture2DArray* Texture2DArrayRHI = Texture->GetTexture2DArray())
		{
			FOpenGLTexture2DArray* Texture2DArray = ResourceCast(Texture2DArrayRHI);
			FOpenGLShaderResourceView *View = 0;

			if (FOpenGL::SupportsTextureView())
			{
				VERIFY_GL_SCOPE();

				GLuint Resource = 0;

				FOpenGL::GenTextures(1, &Resource);
				const FOpenGLTextureFormat& GLFormat = GOpenGLTextureFormats[Texture2DArray->GetFormat()];
				const bool bSRGB = (Texture2DArray->GetFlags()&TexCreate_SRGB) != 0;

				FOpenGL::TextureView(Resource, Texture2DArray->Target, Texture2DArray->GetResource(), GLFormat.InternalFormat[bSRGB], MipLevel, 1, 0, 1);

				return new FOpenGLShaderResourceView(this, Resource, Texture2DArray->Target, MipLevel, true);
			}
			else
			{
				return new FOpenGLShaderResourceView(this, Texture2DArray->GetResource(), Texture2DArray->Target, MipLevel, false);
			}
		}
		else if (FRHITextureCube* TextureCubeRHI = Texture->GetTextureCube())
		{
			FOpenGLTextureCube* TextureCube = ResourceCast(TextureCubeRHI);
			if (FOpenGL::SupportsTextureView())
			{
				VERIFY_GL_SCOPE();

				GLuint Resource = 0;

				FOpenGL::GenTextures(1, &Resource);
				const FOpenGLTextureFormat& GLFormat = GOpenGLTextureFormats[TextureCube->GetFormat()];
				const bool bSRGB = (TextureCube->GetFlags()&TexCreate_SRGB) != 0;

				FOpenGL::TextureView(Resource, TextureCube->Target, TextureCube->GetResource(), GLFormat.InternalFormat[bSRGB], MipLevel, 1, 0, 6);

				return new FOpenGLShaderResourceView(this, Resource, TextureCube->Target, MipLevel, true);
			}
			else
			{
				return new FOpenGLShaderResourceView(this, TextureCube->GetResource(), TextureCube->Target, MipLevel, false);
			}
		}
		else if (FRHITexture3D* Texture3DRHI = Texture->GetTexture3D())
		{
			FOpenGLTexture3D* Texture3D = ResourceCast(Texture3DRHI);

			FOpenGLShaderResourceView *View = 0;

			if (FOpenGL::SupportsTextureView())
			{
				VERIFY_GL_SCOPE();

				GLuint Resource = 0;

				FOpenGL::GenTextures(1, &Resource);
				const FOpenGLTextureFormat& GLFormat = GOpenGLTextureFormats[Texture3D->GetFormat()];
				const bool bSRGB = (Texture3D->GetFlags()&TexCreate_SRGB) != 0;

				FOpenGL::TextureView(Resource, Texture3D->Target, Texture3D->GetResource(), GLFormat.InternalFormat[bSRGB], MipLevel, 1, 0, 1);

				return new FOpenGLShaderResourceView(this, Resource, Texture3D->Target, MipLevel, true);
			}
			else
			{
				return new FOpenGLShaderResourceView(this, Texture3D->GetResource(), Texture3D->Target, MipLevel, false);
			}
		}
		else
		{
			check(false);
			return nullptr;
		}
	});
	return ViewProxy;
}

/** Generates mip maps for the surface. */
void FOpenGLDynamicRHI::RHIGenerateMips(FRHITexture* SurfaceRHI)
{
	if (FOpenGL::SupportsGenerateMipmap())
	{
		RunOnGLRenderContextThread([=]()
		{
			VERIFY_GL_SCOPE();
			GPUProfilingData.RegisterGPUWork(0);

			FOpenGLContextState& ContextState = GetContextStateForCurrentContext();
			FOpenGLTextureBase* Texture = GetOpenGLTextureFromRHITexture(SurfaceRHI);
			// Setup the texture on a disused unit
			// need to figure out how to setup mips properly in no views case
			CachedSetupTextureStage(ContextState, FOpenGL::GetMaxCombinedTextureImageUnits() - 1, Texture->Target, Texture->GetResource(), -1, Texture->NumMips); 
			FOpenGL::GenerateMipmap(Texture->Target);
		});
	}
	else
	{
		UE_LOG( LogRHI, Fatal, TEXT("Generate Mipmaps unsupported on this OpenGL version"));
	}
}



/**
 * Computes the size in memory required by a given texture.
 *
 * @param	TextureRHI		- Texture we want to know the size of
 * @return					- Size in Bytes
 */
uint32 FOpenGLDynamicRHI::RHIComputeMemorySize(FRHITexture* TextureRHI)
{
	if(!TextureRHI)
	{
		return 0;
	}

	FOpenGLTextureBase* Texture = static_cast<FOpenGLTextureBase*>(TextureRHI->GetTextureBaseRHI());
	if (!Texture->IsMemorySizeSet())
	{
		GetOpenGLTextureFromRHITexture(TextureRHI);
	}
	return Texture->GetMemorySize();
}


static FTexture2DRHIRef CreateAsyncReallocate2DTextureTarget(FOpenGLDynamicRHI* OGLRHI, FRHITexture2D* Texture2DRHI, int32 NewMipCountIn, int32 NewSizeX, int32 NewSizeY)
{
	FOpenGLTexture2D* Texture2D = FOpenGLDynamicRHI::ResourceCast(Texture2DRHI);
	uint8 Format = Texture2D->GetFormat();
	uint32 NumSamples = 1;
	ETextureCreateFlags Flags = Texture2D->GetFlags();
	uint32 NewMipCount = (uint32)NewMipCountIn;
	uint32 OriginalMipCount = Texture2DRHI->GetNumMips();
	const FClearValueBinding ClearBinding = Texture2DRHI->GetClearBinding();
	FTexture2DRHIRef NewTexture2DRHI = (FRHITexture2D*)OGLRHI->CreateOpenGLRHITextureOnly(NewSizeX, NewSizeY, false, false, false, Format, NewMipCount, NumSamples, 1, Flags, ClearBinding);

	// CreateOpenGLRHITextureOnly can potentially change some of the input parameters, ensure that's not happening:
	check(Format == (uint8)Texture2D->GetFormat());
	check(Flags == Texture2D->GetFlags());
	check(NumSamples == 1);
	return NewTexture2DRHI;
}

static void GLCopyAsyncTexture2D(FOpenGLDynamicRHI* OGLRHI, FRHITexture2D* NewTexture2DRHI, int32 NewSizeX, int32 NewSizeY, FRHITexture2D* SourceTexture2DRHI, FThreadSafeCounter* RequestStatus)
{
	VERIFY_GL_SCOPE();

	FOpenGLTexture2D* SourceTexture2D = FOpenGLDynamicRHI::ResourceCast(SourceTexture2DRHI);
	uint8 Format = NewTexture2DRHI->GetFormat();
	uint32 NumSamples = 1;
	ETextureCreateFlags Flags = NewTexture2DRHI->GetFlags();
	uint32 NewMipCount = (uint32)NewTexture2DRHI->GetNumMips();
	uint32 SourceMipCount = SourceTexture2DRHI->GetNumMips();

	const FClearValueBinding ClearBinding = NewTexture2DRHI->GetClearBinding();


	OGLRHI->InitializeGLTexture(NewTexture2DRHI, NewSizeX, NewSizeY, false, false, false, Format, NewMipCount, 1, 1, Flags, ClearBinding);

	FOpenGLTexture2D* NewTexture2D = FOpenGLDynamicRHI::ResourceCast(NewTexture2DRHI);

	const uint32 BlockSizeX = GPixelFormats[Format].BlockSizeX;
	const uint32 BlockSizeY = GPixelFormats[Format].BlockSizeY;
	const uint32 NumBytesPerBlock = GPixelFormats[Format].BlockBytes;

	// Use the GPU to asynchronously copy the old mip-maps into the new texture.
	const uint32 NumSharedMips = FMath::Min(SourceMipCount, NewMipCount);
	const uint32 SourceMipOffset = SourceMipCount - NumSharedMips;
	const uint32 DestMipOffset = NewMipCount - NumSharedMips;

	if (FOpenGL::SupportsCopyImage())
	{
		FOpenGLTexture2D *NewOGLTexture2D = (FOpenGLTexture2D*)NewTexture2D;
		FOpenGLTexture2D *OGLTexture2D = (FOpenGLTexture2D*)SourceTexture2D;
		NewOGLTexture2D->CloneViaCopyImage(OGLTexture2D, NumSharedMips, SourceMipOffset, DestMipOffset);
	}
	else
	{
		FOpenGLTexture2D *NewOGLTexture2D = (FOpenGLTexture2D*)NewTexture2D;
		FOpenGLTexture2D *OGLTexture2D = (FOpenGLTexture2D*)SourceTexture2D;
		NewOGLTexture2D->CloneViaPBO(OGLTexture2D, NumSharedMips, SourceMipOffset, DestMipOffset);
	}

	// Decrement the thread-safe counter used to track the completion of the reallocation, since D3D handles sequencing the
	// async mip copies with other D3D calls.
	RequestStatus->Decrement();
}

FTexture2DRHIRef FOpenGLDynamicRHI::AsyncReallocateTexture2D_RenderThread(class FRHICommandListImmediate& RHICmdList, FRHITexture2D* Texture2DRHI, int32 NewMipCountIn, int32 NewSizeX, int32 NewSizeY, FThreadSafeCounter* RequestStatus)
{
	if (RHICmdList.Bypass() || !IsRunningRHIInSeparateThread())
	{
		return RHIAsyncReallocateTexture2D(Texture2DRHI, NewMipCountIn, NewSizeX, NewSizeY, RequestStatus);
	}
	else
	{
		FTexture2DRHIRef NewTexture2DRHI = CreateAsyncReallocate2DTextureTarget(this, Texture2DRHI, NewMipCountIn, NewSizeX, NewSizeY);
		FOpenGLTexture2D* Texture2D = FOpenGLDynamicRHI::ResourceCast(NewTexture2DRHI.GetReference());
		Texture2D->CreationFence.Reset();

		ALLOC_COMMAND_CL(RHICmdList, FRHICommandGLCommand)([=]() 
		{
			GLCopyAsyncTexture2D(this, NewTexture2DRHI, NewSizeX, NewSizeY, Texture2DRHI, RequestStatus); 
			Texture2D->CreationFence.WriteAssertFence();
		});

		Texture2D->CreationFence.SetRHIThreadFence();
		return NewTexture2DRHI;
	}
}

FTexture2DRHIRef FOpenGLDynamicRHI::RHIAsyncReallocateTexture2D(FRHITexture2D* Texture2DRHI, int32 NewMipCountIn, int32 NewSizeX, int32 NewSizeY, FThreadSafeCounter* RequestStatus)
{
	FTexture2DRHIRef NewTexture2DRHI = CreateAsyncReallocate2DTextureTarget(this, Texture2DRHI, NewMipCountIn, NewSizeX, NewSizeY);
	GLCopyAsyncTexture2D(this, NewTexture2DRHI, NewSizeX, NewSizeY, Texture2DRHI, RequestStatus);
	return NewTexture2DRHI;
}

/**
 * Returns the status of an ongoing or completed texture reallocation:
 *	TexRealloc_Succeeded	- The texture is ok, reallocation is not in progress.
 *	TexRealloc_Failed		- The texture is bad, reallocation is not in progress.
 *	TexRealloc_InProgress	- The texture is currently being reallocated async.
 *
 * @param Texture2D		- Texture to check the reallocation status for
 * @return				- Current reallocation status
 */
ETextureReallocationStatus FOpenGLDynamicRHI::RHIFinalizeAsyncReallocateTexture2D(FRHITexture2D* Texture2D, bool bBlockUntilCompleted )
{
	return TexRealloc_Succeeded;
}

/**
 * Cancels an async reallocation for the specified texture.
 * This should be called for the new texture, not the original.
 *
 * @param Texture				Texture to cancel
 * @param bBlockUntilCompleted	If true, blocks until the cancellation is fully completed
 * @return						Reallocation status
 */
ETextureReallocationStatus FOpenGLDynamicRHI::RHICancelAsyncReallocateTexture2D(FRHITexture2D* Texture2D, bool bBlockUntilCompleted )
{
	return TexRealloc_Succeeded;
}

void* FOpenGLDynamicRHI::RHILockTexture2D(FRHITexture2D* TextureRHI,uint32 MipIndex,EResourceLockMode LockMode,uint32& DestStride,bool bLockWithinMiptail)
{
	FOpenGLTexture2D* Texture = ResourceCast(TextureRHI);
	return Texture->Lock(MipIndex,0,LockMode,DestStride);
}

void FOpenGLDynamicRHI::RHIUnlockTexture2D(FRHITexture2D* TextureRHI,uint32 MipIndex,bool bLockWithinMiptail)
{
	FOpenGLTexture2D* Texture = ResourceCast(TextureRHI);
	Texture->Unlock(MipIndex, 0);
}

void* FOpenGLDynamicRHI::RHILockTexture2DArray(FRHITexture2DArray* TextureRHI,uint32 TextureIndex,uint32 MipIndex,EResourceLockMode LockMode,uint32& DestStride,bool bLockWithinMiptail)
{
	FOpenGLTexture2DArray* Texture = ResourceCast(TextureRHI);
	return Texture->Lock(MipIndex,TextureIndex,LockMode,DestStride);
}

void FOpenGLDynamicRHI::RHIUnlockTexture2DArray(FRHITexture2DArray* TextureRHI,uint32 TextureIndex,uint32 MipIndex,bool bLockWithinMiptail)
{
	FOpenGLTexture2DArray* Texture = ResourceCast(TextureRHI);
	Texture->Unlock(MipIndex, TextureIndex);
}

void FOpenGLDynamicRHI::RHIUpdateTexture2D(FRHITexture2D* TextureRHI,uint32 MipIndex,const FUpdateTextureRegion2D& UpdateRegionIn,uint32 SourcePitch,const uint8* SourceDataIn)
{
	FRHICommandListImmediate& RHICmdList = FRHICommandListExecutor::GetImmediateCommandList();
	const FUpdateTextureRegion2D UpdateRegion = UpdateRegionIn;

	uint8* RHITSourceData = nullptr;
	if (!ShouldRunGLRenderContextOpOnThisThread(RHICmdList))
	{
		const FPixelFormatInfo& FormatInfo = GPixelFormats[TextureRHI->GetFormat()];
		const size_t UpdateHeightInTiles = FMath::DivideAndRoundUp(UpdateRegion.Height, (uint32)FormatInfo.BlockSizeY);
		const size_t DataSize = static_cast<size_t>(SourcePitch) * UpdateHeightInTiles;
		RHITSourceData = (uint8*)FMemory::Malloc(DataSize, 16);
		FMemory::Memcpy(RHITSourceData, SourceDataIn, DataSize);
	}
	const uint8* SourceData = RHITSourceData ? RHITSourceData : SourceDataIn;
	RunOnGLRenderContextThread([=]()
	{
		VERIFY_GL_SCOPE();

		FOpenGLTexture2D* Texture = ResourceCast(TextureRHI);

		// Use a texture stage that's not likely to be used for draws, to avoid waiting
		FOpenGLContextState& ContextState = GetContextStateForCurrentContext();
		CachedSetupTextureStage(ContextState, FOpenGL::GetMaxCombinedTextureImageUnits() - 1, Texture->Target, Texture->GetResource(), 0, Texture->GetNumMips());
		CachedBindPixelUnpackBuffer(ContextState, 0);

		EPixelFormat PixelFormat = Texture->GetFormat();
		check(GPixelFormats[PixelFormat].BlockSizeX == 1);
		check(GPixelFormats[PixelFormat].BlockSizeY == 1);
		const FOpenGLTextureFormat& GLFormat = GOpenGLTextureFormats[PixelFormat];
		const uint32 FormatBPP = GPixelFormats[PixelFormat].BlockBytes;
		checkf(!GLFormat.bCompressed, TEXT("RHIUpdateTexture2D not currently supported for compressed (%s) textures by the OpenGL RHI"), GPixelFormats[PixelFormat].Name);

		glPixelStorei(GL_UNPACK_ROW_LENGTH, SourcePitch / FormatBPP);

		glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
		glTexSubImage2D(Texture->Target, MipIndex, UpdateRegion.DestX, UpdateRegion.DestY, UpdateRegion.Width, UpdateRegion.Height,
			GLFormat.Format, GLFormat.Type, SourceData);
		glPixelStorei(GL_UNPACK_ALIGNMENT, 4);

		glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);

		// No need to restore texture stage; leave it like this,
		// and the next draw will take care of cleaning it up; or
		// next operation that needs the stage will switch something else in on it.

		// free source data if we're on RHIT
		if (RHITSourceData)
		{
			FMemory::Free(RHITSourceData);
		}
	});
}

void FOpenGLDynamicRHI::RHIUpdateTexture3D(FRHITexture3D* TextureRHI, uint32 MipIndex, const FUpdateTextureRegion3D& UpdateRegion, uint32 SourceRowPitch, uint32 SourceDepthPitch, const uint8* SourceData)
{
	VERIFY_GL_SCOPE();
	check( FOpenGL::SupportsTexture3D() );
	FOpenGLTexture3D* Texture = ResourceCast(TextureRHI);

	// Use a texture stage that's not likely to be used for draws, to avoid waiting
	FOpenGLContextState& ContextState = GetContextStateForCurrentContext();
	CachedSetupTextureStage(ContextState, FOpenGL::GetMaxCombinedTextureImageUnits() - 1, Texture->Target, Texture->GetResource(), 0, Texture->GetNumMips());
	CachedBindPixelUnpackBuffer(ContextState, 0);

	EPixelFormat PixelFormat = Texture->GetFormat();
	const FOpenGLTextureFormat& GLFormat = GOpenGLTextureFormats[PixelFormat];
	const FPixelFormatInfo& FormatInfo = GPixelFormats[PixelFormat];
	const uint32 FormatBPP = FormatInfo.BlockBytes;

	check( FOpenGL::SupportsTexture3D() );
	// TO DO - add appropriate offsets to source data when necessary
	check(UpdateRegion.SrcX == 0);
	check(UpdateRegion.SrcY == 0);
	check(UpdateRegion.SrcZ == 0);
	
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

	const bool bSRGB = (Texture->GetFlags() & TexCreate_SRGB) != 0;

	if (GLFormat.bCompressed)
	{
		FOpenGL::CompressedTexSubImage3D(
			Texture->Target,
			MipIndex,
			UpdateRegion.DestX, UpdateRegion.DestY, UpdateRegion.DestZ,
			UpdateRegion.Width, UpdateRegion.Height, UpdateRegion.Depth,
			GLFormat.InternalFormat[bSRGB],
			SourceDepthPitch * UpdateRegion.Depth,
			SourceData);
	}
	else
	{
		glPixelStorei(GL_UNPACK_ROW_LENGTH, UpdateRegion.Width / FormatInfo.BlockSizeX);
		glPixelStorei(GL_UNPACK_IMAGE_HEIGHT, UpdateRegion.Height / FormatInfo.BlockSizeY);

		FOpenGL::TexSubImage3D(Texture->Target, MipIndex, UpdateRegion.DestX, UpdateRegion.DestY, UpdateRegion.DestZ, UpdateRegion.Width, UpdateRegion.Height, UpdateRegion.Depth, GLFormat.Format, GLFormat.Type, SourceData);
	}

	glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
	glPixelStorei(GL_UNPACK_IMAGE_HEIGHT, 0);
	glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);


	// No need to restore texture stage; leave it like this,
	// and the next draw will take care of cleaning it up; or
	// next operation that needs the stage will switch something else in on it.
}

void FOpenGLDynamicRHI::InvalidateTextureResourceInCache(GLuint Resource)
{
	VERIFY_GL_SCOPE();
	if (SharedContextState.Textures || RenderingContextState.Textures || PendingState.Textures)
	{
		for (int32 SamplerIndex = 0; SamplerIndex < FOpenGL::GetMaxCombinedTextureImageUnits(); ++SamplerIndex)
		{
			if (SharedContextState.Textures && SharedContextState.Textures[SamplerIndex].Resource == Resource)
			{
				SharedContextState.Textures[SamplerIndex].Target = GL_NONE;
				SharedContextState.Textures[SamplerIndex].Resource = 0;
			}

			if (RenderingContextState.Textures && RenderingContextState.Textures[SamplerIndex].Resource == Resource)
			{
				RenderingContextState.Textures[SamplerIndex].Target = GL_NONE;
				RenderingContextState.Textures[SamplerIndex].Resource = 0;
			}

			if (PendingState.Textures && PendingState.Textures[SamplerIndex].Resource == Resource)
			{
				PendingState.Textures[SamplerIndex].Target = GL_NONE;
				PendingState.Textures[SamplerIndex].Resource = 0;
			}
		}
	}
	
	TextureMipLimits.Remove(Resource);
	
	if (PendingState.DepthStencil && PendingState.DepthStencil->GetResource() == Resource)
	{
		PendingState.DepthStencil = nullptr;
	}
}

void FOpenGLDynamicRHI::InvalidateUAVResourceInCache(GLuint Resource)
{
	VERIFY_GL_SCOPE();
	for (int32 UAVIndex = 0; UAVIndex < FOpenGL::GetMaxCombinedUAVUnits(); ++UAVIndex)
	{
		if (SharedContextState.UAVs[UAVIndex].Resource == Resource)
		{
			SharedContextState.UAVs[UAVIndex].Format = GL_NONE;
			SharedContextState.UAVs[UAVIndex].Resource = 0;
		}

		if (RenderingContextState.UAVs[UAVIndex].Resource == Resource)
		{
			RenderingContextState.UAVs[UAVIndex].Format = GL_NONE;
			RenderingContextState.UAVs[UAVIndex].Resource = 0;
		}

		if (PendingState.UAVs[UAVIndex].Resource == Resource)
		{
			PendingState.UAVs[UAVIndex].Format = GL_NONE;
			PendingState.UAVs[UAVIndex].Resource = 0;
		}
	}
}

/*-----------------------------------------------------------------------------
	Cubemap texture support.
-----------------------------------------------------------------------------*/
FTextureCubeRHIRef FOpenGLDynamicRHI::RHICreateTextureCube( uint32 Size, uint8 Format, uint32 NumMips, ETextureCreateFlags Flags, ERHIAccess InResourceState, FRHIResourceCreateInfo& CreateInfo )
{
	// not yet supported
	check(!CreateInfo.BulkData);

	return (FRHITextureCube*)CreateOpenGLTexture(Size,Size,true, false, false, Format, NumMips, 1, 1, Flags, CreateInfo.ClearValueBinding);
}

FTextureCubeRHIRef FOpenGLDynamicRHI::RHICreateTextureCubeArray( uint32 Size, uint32 ArraySize, uint8 Format, uint32 NumMips, ETextureCreateFlags Flags, ERHIAccess InResourceState, FRHIResourceCreateInfo& CreateInfo )
{
	// not yet supported
	check(!CreateInfo.BulkData);

	return (FRHITextureCube*)CreateOpenGLTexture(Size, Size, true, true, false, Format, NumMips, 1, 6 * ArraySize, Flags, CreateInfo.ClearValueBinding);
}

void* FOpenGLDynamicRHI::RHILockTextureCubeFace(FRHITextureCube* TextureCubeRHI,uint32 FaceIndex,uint32 ArrayIndex,uint32 MipIndex,EResourceLockMode LockMode,uint32& DestStride,bool bLockWithinMiptail)
{
	FOpenGLTextureCube* TextureCube = ResourceCast(TextureCubeRHI);
	return TextureCube->Lock(MipIndex,FaceIndex + 6 * ArrayIndex,LockMode,DestStride);
}

void FOpenGLDynamicRHI::RHIUnlockTextureCubeFace(FRHITextureCube* TextureCubeRHI,uint32 FaceIndex,uint32 ArrayIndex,uint32 MipIndex,bool bLockWithinMiptail)
{
	FOpenGLTextureCube* TextureCube = ResourceCast(TextureCubeRHI);
	TextureCube->Unlock(MipIndex,FaceIndex + ArrayIndex * 6);
}

void FOpenGLDynamicRHI::RHIBindDebugLabelName(FRHITexture* TextureRHI, const TCHAR* Name)
{
#if GLDEBUG_LABELS_ENABLED
	FRHICommandListImmediate& RHICmdList = FRHICommandListExecutor::GetImmediateCommandList();
	if (ShouldRunGLRenderContextOpOnThisThread(RHICmdList))
	{
		VERIFY_GL_SCOPE();
		FOpenGLTextureBase* Texture = GetOpenGLTextureFromRHITexture(TextureRHI);
		if (Texture->IsEvicted())
		{
			Texture->EvictionParamsPtr->SetDebugLabelName(TCHAR_TO_ANSI(Name));
		}
		else
		{
			FOpenGL::LabelObject(GL_TEXTURE, Texture->GetResource(), TCHAR_TO_ANSI(Name));
		}
	}
	else
	{
		// copy string name for RHIT version.
		FAnsiCharArray TextureDebugName;
		TextureDebugName.Append(TCHAR_TO_ANSI(Name), FCString::Strlen(Name) + 1);
		RunOnGLRenderContextThread([TextureRHI, TextureDebugName = MoveTemp(TextureDebugName)]()
		{
			VERIFY_GL_SCOPE();
			FOpenGLTextureBase* Texture = GetOpenGLTextureFromRHITexture(TextureRHI);
			if (Texture->IsEvicted())
			{
				Texture->EvictionParamsPtr->SetDebugLabelName(TextureDebugName);
			}
			else
			{
				FOpenGL::LabelObject(GL_TEXTURE, Texture->GetResource(), TextureDebugName.GetData());
			}
		});
	}
#endif
}


void FOpenGLDynamicRHI::RHIVirtualTextureSetFirstMipInMemory(FRHITexture2D* TextureRHI, uint32 FirstMip)
{
}

void FOpenGLDynamicRHI::RHIVirtualTextureSetFirstMipVisible(FRHITexture2D* TextureRHI, uint32 FirstMip)
{
}

FTextureReferenceRHIRef FOpenGLDynamicRHI::RHICreateTextureReference(FLastRenderTimeContainer* InLastRenderTime)
{
	return new FOpenGLTextureReference(InLastRenderTime);
}

void FOpenGLTextureReference::SetReferencedTexture(FRHITexture* InTexture)
{
	FRHITextureReference::SetReferencedTexture(InTexture);
	TexturePtr = GetOpenGLTextureFromRHITexture(InTexture);
}

void FOpenGLDynamicRHI::RHIUpdateTextureReference(FRHITextureReference* TextureRefRHI, FRHITexture* NewTextureRHI)
{
	auto* TextureRef = (FOpenGLTextureReference*)TextureRefRHI;
	if (TextureRef)
	{
		TextureRef->SetReferencedTexture(NewTextureRHI);
	}
}

void FOpenGLDynamicRHI::RHICopySubTextureRegion(FRHITexture2D* SourceTextureRHI, FRHITexture2D* DestinationTextureRHI, FBox2D SourceBox, FBox2D DestinationBox)
{
	VERIFY_GL_SCOPE();
	FOpenGLTexture2D* SourceTexture = ResourceCast(SourceTextureRHI);
	FOpenGLTexture2D* DestinationTexture = ResourceCast(DestinationTextureRHI);

	check(SourceTexture->Target == DestinationTexture->Target);

	// Use a texture stage that's not likely to be used for draws, to avoid waiting
	FOpenGLContextState& ContextState = GetContextStateForCurrentContext();
	CachedSetupTextureStage(ContextState, FOpenGL::GetMaxCombinedTextureImageUnits() - 1, DestinationTexture->Target, DestinationTexture->GetResource(), 0, DestinationTexture->GetNumMips());
	CachedBindPixelUnpackBuffer(ContextState, 0);

	// Convert sub texture regions to GL types
	GLint XOffset = DestinationBox.Min.X;
	GLint YOffset = DestinationBox.Min.Y;
	GLint X = SourceBox.Min.X;
	GLint Y = SourceBox.Min.Y;
	GLsizei Width = DestinationBox.Max.X - DestinationBox.Min.X;
	GLsizei Height = DestinationBox.Max.Y - DestinationBox.Min.Y;

	// Bind source texture to an FBO to read from
	FOpenGLTextureBase* RenderTarget[] = { SourceTexture };
	uint32 MipLevel = 0;
	GLuint SourceFBO = GetOpenGLFramebuffer(1, RenderTarget, NULL, &MipLevel, NULL);
	check(SourceFBO != 0);

	glBindFramebuffer(GL_FRAMEBUFFER, SourceFBO);

	FOpenGL::ReadBuffer(GL_COLOR_ATTACHMENT0);
	FOpenGL::CopyTexSubImage2D(DestinationTexture->Target, 0, XOffset, YOffset, X, Y, Width, Height);

	ContextState.Framebuffer = (GLuint)-1;
}


void FOpenGLDynamicRHI::RHICopyTexture(FRHITexture* SourceTextureRHI, FRHITexture* DestTextureRHI, const FRHICopyTextureInfo& CopyInfo)
{
	VERIFY_GL_SCOPE();
	FOpenGLTextureBase* SourceTexture = GetOpenGLTextureFromRHITexture(SourceTextureRHI);
	FOpenGLTextureBase* DestTexture = GetOpenGLTextureFromRHITexture(DestTextureRHI);

	checkf(SourceTexture->Target == DestTexture->Target, TEXT("Cannot copy between different texture targets, SourceTexture Target=%x, Format=%d, Flags=%x; DestTexture Target=%x, Format=%d, Flags=%x"),
		SourceTexture->Target, SourceTextureRHI->GetFormat(), SourceTextureRHI->GetFlags(),
		DestTexture->Target, DestTextureRHI->GetFormat(), DestTextureRHI->GetFlags()
	);
	
	checkf((SourceTextureRHI->GetFlags() & TexCreate_SRGB) == (DestTextureRHI->GetFlags() & TexCreate_SRGB), TEXT("Cannot copy between sRGB and linear, SourceTexture Format=%d, Flags=%x; DestTexture Format=%d, Flags=%x"),
		SourceTextureRHI->GetFormat(), SourceTextureRHI->GetFlags(),
		DestTextureRHI->GetFormat(), DestTextureRHI->GetFlags()
	);

	GLsizei Width, Height, Depth;

	if (CopyInfo.Size == FIntVector::ZeroValue)
	{
		// Copy whole texture when zero vector is specified for region size.
		FIntVector SrcTexSize = SourceTextureRHI->GetSizeXYZ();
		Width = FMath::Max(1, SrcTexSize.X >> CopyInfo.SourceMipIndex);
		Height = FMath::Max(1, SrcTexSize.Y >> CopyInfo.SourceMipIndex);
		switch (SourceTexture->Target)
		{
		case GL_TEXTURE_3D:			Depth = FMath::Max(1, SrcTexSize.Z >> CopyInfo.SourceMipIndex); break;
		case GL_TEXTURE_CUBE_MAP:	Depth = 6; break;
		default:					Depth = 1; break;
		}
		ensure(CopyInfo.SourcePosition == FIntVector::ZeroValue);
	}
	else
	{
		Width = CopyInfo.Size.X;
		Height = CopyInfo.Size.Y;
		switch (SourceTexture->Target)
		{
			case GL_TEXTURE_3D:			Depth = CopyInfo.Size.Z; break;
			case GL_TEXTURE_CUBE_MAP:	Depth = CopyInfo.NumSlices; break;
			default:					Depth = 1; break;
		}
	}

	GLint SrcMip = CopyInfo.SourceMipIndex;
	GLint DestMip = CopyInfo.DestMipIndex;

	if (FOpenGL::SupportsCopyImage())
	{
		GLint SrcZOffset, DestZOffset;
		switch (SourceTexture->Target)
		{
		case GL_TEXTURE_3D:
		case GL_TEXTURE_CUBE_MAP:
			// For cube maps, the Z offsets select the starting faces.
			SrcZOffset = CopyInfo.SourcePosition.Z;
			DestZOffset = CopyInfo.DestPosition.Z;
			break;
		case GL_TEXTURE_1D_ARRAY:
		case GL_TEXTURE_2D_ARRAY:
			// For texture arrays, the Z offsets and depth actually refer to the range of slices to copy.
			SrcZOffset = CopyInfo.SourceSliceIndex;
			DestZOffset = CopyInfo.DestSliceIndex;
			Depth = CopyInfo.NumSlices;
			break;
		default:
			SrcZOffset = 0;
			DestZOffset = 0;
			break;
		}

		for (uint32 MipIndex = 0; MipIndex < CopyInfo.NumMips; ++MipIndex)
		{
			FOpenGL::CopyImageSubData(SourceTexture->GetResource(), SourceTexture->Target, SrcMip, CopyInfo.SourcePosition.X, CopyInfo.SourcePosition.Y, SrcZOffset,
				DestTexture->GetResource(), DestTexture->Target, DestMip, CopyInfo.DestPosition.X, CopyInfo.DestPosition.Y, DestZOffset,
				Width, Height, Depth);

			++SrcMip;
			++DestMip;

			Width = FMath::Max(1, Width >> 1);
			Height = FMath::Max(1, Height >> 1);
			if(DestTexture->Target == GL_TEXTURE_3D)
			{
				Depth = FMath::Max(1, Depth >> 1);
			}
		}

		return;
	}
	
	// Convert sub texture regions to GL types
	GLint XOffset = CopyInfo.DestPosition.X;
	GLint YOffset = CopyInfo.DestPosition.Y;
	GLint ZOffset = CopyInfo.DestPosition.Z;
	GLint X = CopyInfo.SourcePosition.X;
	GLint Y = CopyInfo.SourcePosition.Y;
	GLint Z = CopyInfo.SourcePosition.Z;

	// Use a texture stage that's not likely to be used for draws, to avoid waiting
	FOpenGLContextState& ContextState = GetContextStateForCurrentContext();
	CachedSetupTextureStage(ContextState, FOpenGL::GetMaxCombinedTextureImageUnits() - 1, DestTexture->Target, DestTexture->GetResource(), 0, DestTextureRHI->GetNumMips());
	CachedBindPixelUnpackBuffer(ContextState, 0);

	// Bind source texture to an FBO to read from
	for (uint32 SliceIndex = 0; SliceIndex < CopyInfo.NumSlices; ++SliceIndex)
	{
		for (uint32 MipIndex = 0; MipIndex < CopyInfo.NumMips; ++MipIndex)
		{
			FOpenGLTextureBase* RenderTargets[1] = { SourceTexture };
			uint32 MipLevels[1] = { static_cast<uint32>(SrcMip) };
			uint32 ArrayIndices[1] = { CopyInfo.SourceSliceIndex + SliceIndex };

			GLuint SourceFBO = GetOpenGLFramebuffer(1, RenderTargets, ArrayIndices, MipLevels, nullptr);
			check(SourceFBO != 0);

			glBindFramebuffer(GL_FRAMEBUFFER, SourceFBO);

			FOpenGL::ReadBuffer(GL_COLOR_ATTACHMENT0);

			switch (DestTexture->Target)
			{
				case GL_TEXTURE_1D:
					FOpenGL::CopyTexSubImage1D(DestTexture->Target, DestMip, XOffset, X, 0, Width);
					break;
				case GL_TEXTURE_1D_ARRAY:
					FOpenGL::CopyTexSubImage2D(DestTexture->Target, DestMip, XOffset, CopyInfo.DestSliceIndex + SliceIndex, X, 0, Width, 1);
					break;
				case GL_TEXTURE_2D:
				case GL_TEXTURE_RECTANGLE:
					FOpenGL::CopyTexSubImage2D(DestTexture->Target, DestMip, XOffset, YOffset, X, Y, Width, Height);
					break;
				case GL_TEXTURE_2D_ARRAY:
					FOpenGL::CopyTexSubImage3D(DestTexture->Target, DestMip, XOffset, YOffset, CopyInfo.DestSliceIndex + SliceIndex, X, Y, Width, Height);
					break;
				case GL_TEXTURE_3D:
					FOpenGL::CopyTexSubImage3D(DestTexture->Target, DestMip, XOffset, YOffset, ZOffset, X, Y, Width, Height);
					break;
				case GL_TEXTURE_CUBE_MAP:
					for (int32 FaceIndex = FMath::Min((int32)CopyInfo.NumSlices, 6) - 1; FaceIndex >= 0; FaceIndex--)
					{
						FOpenGL::CopyTexSubImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + (uint32)FaceIndex, CopyInfo.DestMipIndex, XOffset, YOffset, X, Y, Width, Height);
					}					
					break;
			}
		}

		++SrcMip;
		++DestMip;

		Width = FMath::Max(1, Width >> 1);
		Height = FMath::Max(1, Height >> 1);
	}

	ContextState.Framebuffer = (GLuint)-1;
}


FTexture2DRHIRef FOpenGLDynamicRHI::RHICreateTexture2DFromResource(EPixelFormat Format, uint32 SizeX, uint32 SizeY, uint32 NumMips, uint32 NumSamples, uint32 NumSamplesTileMem, const FClearValueBinding& ClearValueBinding, GLuint Resource, ETextureCreateFlags TexCreateFlags)
{
	FOpenGLTexture2D* Texture2D = new FOpenGLTexture2D(
		this,
		Resource,
		(NumSamples > 1) ? GL_TEXTURE_2D_MULTISAMPLE : GL_TEXTURE_2D,
		GL_NONE,
		SizeX,
		SizeY,
		0,
		NumMips,
		NumSamples,
		NumSamplesTileMem,
		1,
		Format,
		false,
		false,
		TexCreateFlags,
		ClearValueBinding);

	Texture2D->SetAliased(true);
	OpenGLTextureAllocated(Texture2D, TexCreateFlags);
	return Texture2D;
}

FTexture2DRHIRef FOpenGLDynamicRHI::RHICreateTexture2DArrayFromResource(EPixelFormat Format, uint32 SizeX, uint32 SizeY, uint32 ArraySize, uint32 NumMips, uint32 NumSamples, uint32 NumSamplesTileMem, const FClearValueBinding& ClearValueBinding, GLuint Resource, ETextureCreateFlags TexCreateFlags)
{
	FOpenGLTexture2D* Texture2DArray = new FOpenGLTexture2D(
		this,
		Resource,
		GL_TEXTURE_2D_ARRAY,
		GL_NONE,
		SizeX,
		SizeY,
		0,
		NumMips,
		NumSamples,
		NumSamplesTileMem,
		ArraySize,
		Format,
		false,
		false,
		TexCreateFlags,
		ClearValueBinding);

	Texture2DArray->SetAliased(true);
	OpenGLTextureAllocated(Texture2DArray, TexCreateFlags);
	return Texture2DArray;
}

FTextureCubeRHIRef FOpenGLDynamicRHI::RHICreateTextureCubeFromResource(EPixelFormat Format, uint32 Size, bool bArray, uint32 ArraySize, uint32 NumMips, uint32 NumSamples, uint32 NumSamplesTileMem, const FClearValueBinding& ClearValueBinding, GLuint Resource, ETextureCreateFlags TexCreateFlags)
{
	FOpenGLTextureCube* TextureCube = new FOpenGLTextureCube(
		this,
		Resource,
		GL_TEXTURE_CUBE_MAP,
		GL_NONE,
		Size,
		Size,
		0,
		NumMips,
		NumSamples,
		NumSamplesTileMem,
		1,
		Format,
		false,
		false,
		TexCreateFlags,
		ClearValueBinding);

	TextureCube->SetAliased(true);
	OpenGLTextureAllocated(TextureCube, TexCreateFlags);
	return TextureCube;
}

PRAGMA_DISABLE_DEPRECATION_WARNINGS

void FOpenGLDynamicRHI::RHIAliasTextureResources(FTextureRHIRef& DestRHITexture, FTextureRHIRef& SrcRHITexture)
{
	// @todo: Move the raw-pointer implementation down here when it's deprecation is completed.
	RHIAliasTextureResources((FRHITexture*)DestRHITexture, (FRHITexture*)SrcRHITexture);
}

FTextureRHIRef FOpenGLDynamicRHI::RHICreateAliasedTexture(FTextureRHIRef& SourceTexture)
{
	// @todo: Move the raw-pointer implementation down here when it's deprecation is completed.
	return RHICreateAliasedTexture((FRHITexture*)SourceTexture);
}

void FOpenGLDynamicRHI::RHIAliasTextureResources(FRHITexture* DestRHITexture, FRHITexture* SrcRHITexture)
{
	VERIFY_GL_SCOPE();
	FOpenGLTextureBase* DestTexture = GetOpenGLTextureFromRHITexture(DestRHITexture);
	FOpenGLTextureBase* SrcTexture = GetOpenGLTextureFromRHITexture(SrcRHITexture);

	if (DestTexture && SrcTexture)
	{
		DestTexture->AliasResources(SrcTexture);
	}
}

FRHITexture* FOpenGLDynamicRHI::CreateTexture2DAliased(FOpenGLTexture2D* SourceTexture)
{
	SCOPE_CYCLE_COUNTER(STAT_OpenGLCreateTextureTime);

	FRHITexture* Result = new FOpenGLTexture2D(this, 0, SourceTexture->Target, -1, SourceTexture->GetSizeX(), SourceTexture->GetSizeY(), 0, 
		SourceTexture->GetNumMips(), SourceTexture->GetNumSamples(), SourceTexture->GetNumSamplesTileMem(), 1, SourceTexture->GetFormat(), 
		false, false, SourceTexture->GetFlags(), SourceTexture->GetClearBinding());

	RHIAliasTextureResources(Result, SourceTexture);

	return Result;
}

FRHITexture* FOpenGLDynamicRHI::CreateTexture2DArrayAliased(FOpenGLTexture2DArray* SourceTexture)
{
	SCOPE_CYCLE_COUNTER(STAT_OpenGLCreateTextureTime);

	FRHITexture* Result = new FOpenGLTexture2DArray(this, 0, SourceTexture->Target, -1, SourceTexture->GetSizeX(), SourceTexture->GetSizeY(), SourceTexture->GetSizeZ(),
		SourceTexture->GetNumMips(), SourceTexture->GetNumSamples(), 1 /* aka check(InNumSamplesTileMem == 1) in OpenGLResource.h FOpenGLBaseTexture2DArray constructor */,
		1, SourceTexture->GetFormat(), false, false, SourceTexture->GetFlags(), SourceTexture->GetClearBinding());

	RHIAliasTextureResources(Result, SourceTexture);

	return Result;
}

FRHITexture* FOpenGLDynamicRHI::CreateTextureCubeAliased(FOpenGLTextureCube* SourceTexture)
{
	SCOPE_CYCLE_COUNTER(STAT_OpenGLCreateTextureTime);

	FRHITexture* Result = new FOpenGLTextureCube(this, 0, SourceTexture->Target, -1, SourceTexture->GetSizeX(), SourceTexture->GetSizeY(), SourceTexture->GetSizeZ(),
		SourceTexture->GetNumMips(), SourceTexture->GetNumSamples(), 1 /* OpenGL currently doesn't support multisample cube textures, per OpenGLResource.h FOpenGLBaseTextureCube */,
		1, SourceTexture->GetFormat(), true, false, SourceTexture->GetFlags(), SourceTexture->GetClearBinding());

	RHIAliasTextureResources(Result, SourceTexture);

	return Result;
}

FTextureRHIRef FOpenGLDynamicRHI::RHICreateAliasedTexture(FRHITexture* SourceTexture)
{
	FTextureRHIRef AliasedTexture;
	if (SourceTexture->GetTexture2D() != nullptr)
	{
		AliasedTexture = CreateTexture2DAliased(static_cast<FOpenGLTexture2D*>(SourceTexture));
	}
	else if (SourceTexture->GetTexture2DArray() != nullptr)
	{
		AliasedTexture = CreateTexture2DArrayAliased(static_cast<FOpenGLTexture2DArray*>(SourceTexture));
	}
	else if (SourceTexture->GetTextureCube() != nullptr)
	{
		AliasedTexture = CreateTextureCubeAliased(static_cast<FOpenGLTextureCube*>(SourceTexture));
	}
	else
	{
		UE_LOG(LogRHI, Error, TEXT("Currently FOpenGLDynamicRHI::RHICreateAliasedTexture only supports 2D, 2D Array and Cube textures."));
	}

	if (AliasedTexture)
	{
		FOpenGLTextureBase* BaseTexture = GetOpenGLTextureFromRHITexture(AliasedTexture);
		// Init memory size to zero, since we're aliased.
		BaseTexture->SetMemorySize(0);
	}

	return AliasedTexture;
}

PRAGMA_ENABLE_DEPRECATION_WARNINGS

void* FOpenGLDynamicRHI::LockTexture2D_RenderThread(class FRHICommandListImmediate& RHICmdList, FRHITexture2D* Texture, uint32 MipIndex, EResourceLockMode LockMode, uint32& DestStride, bool bLockWithinMiptail, bool bNeedsDefaultRHIFlush)
{
	check(IsInRenderingThread());
	static auto* CVarRHICmdBufferWriteLocks = IConsoleManager::Get().FindTConsoleVariableDataInt(TEXT("r.RHICmdBufferWriteLocks"));
	bool bBuffer = CVarRHICmdBufferWriteLocks->GetValueOnRenderThread() > 0;
	void* Result;
	uint32 MipBytes = 0;
	if (!bBuffer || LockMode != RLM_WriteOnly || RHICmdList.Bypass() || !IsRunningRHIInSeparateThread())
	{
		RHITHREAD_GLCOMMAND_PROLOGUE();
		return this->RHILockTexture2D(Texture, MipIndex, LockMode, DestStride, bLockWithinMiptail);
		RHITHREAD_GLCOMMAND_EPILOGUE_GET_RETURN(void *);
		Result = ReturnValue;
		MipBytes = ResourceCast_Unfenced(Texture)->GetLockSize(MipIndex, 0, LockMode, DestStride);
	}
	else
	{
		MipBytes = ResourceCast_Unfenced(Texture)->GetLockSize(MipIndex, 0, LockMode, DestStride);
		Result = FMemory::Malloc(MipBytes, 16);
	}
	check(Result);

	GLLockTracker.Lock(Texture, Result, 0, MipIndex, DestStride, MipBytes, LockMode);
	return Result;
}

void FOpenGLDynamicRHI::UnlockTexture2D_RenderThread(class FRHICommandListImmediate& RHICmdList, FRHITexture2D* Texture, uint32 MipIndex, bool bLockWithinMiptail, bool bNeedsDefaultRHIFlush)
{
	check(IsInRenderingThread());
	static auto* CVarRHICmdBufferWriteLocks = IConsoleManager::Get().FindTConsoleVariableDataInt(TEXT("r.RHICmdBufferWriteLocks"));
	bool bBuffer = CVarRHICmdBufferWriteLocks->GetValueOnRenderThread() > 0;
	FTextureLockTracker::FLockParams Params = GLLockTracker.Unlock(Texture, 0, MipIndex);
	if (!bBuffer || Params.LockMode != RLM_WriteOnly || RHICmdList.Bypass() || !IsRunningRHIInSeparateThread())
	{
		GLLockTracker.TotalMemoryOutstanding = 0;
		RHITHREAD_GLCOMMAND_PROLOGUE();
		this->RHIUnlockTexture2D(Texture, MipIndex, bLockWithinMiptail);
		RHITHREAD_GLCOMMAND_EPILOGUE();
	}
	else
	{
		auto GLCommand = [=]()
		{
			uint32 DestStride;
			uint8* TexMem = (uint8*)this->RHILockTexture2D(Texture, MipIndex, Params.LockMode, DestStride, bLockWithinMiptail);
			uint8* BuffMem = (uint8*)Params.Buffer;
			check(DestStride == Params.Stride);
			FMemory::Memcpy(TexMem, BuffMem, Params.BufferSize);
			FMemory::Free(Params.Buffer);
			this->RHIUnlockTexture2D(Texture, MipIndex, bLockWithinMiptail);
		};
		ALLOC_COMMAND_CL(RHICmdList, FRHICommandGLCommand)(MoveTemp(GLCommand));
	}
}

void* FOpenGLDynamicRHI::RHILockTextureCubeFace_RenderThread(class FRHICommandListImmediate& RHICmdList, FRHITextureCube* Texture, uint32 FaceIndex, uint32 ArrayIndex, uint32 MipIndex, EResourceLockMode LockMode, uint32& DestStride, bool bLockWithinMiptail)
{
	check(IsInRenderingThread());
	static auto* CVarRHICmdBufferWriteLocks = IConsoleManager::Get().FindTConsoleVariableDataInt(TEXT("r.RHICmdBufferWriteLocks"));
	bool bBuffer = CVarRHICmdBufferWriteLocks->GetValueOnRenderThread() > 0;
	void* Result;
	uint32 MipBytes = 0;
	if (!bBuffer || LockMode != RLM_WriteOnly || RHICmdList.Bypass() || !IsRunningRHIInSeparateThread())
	{
		RHITHREAD_GLCOMMAND_PROLOGUE();
		return this->RHILockTextureCubeFace(Texture, FaceIndex, ArrayIndex, MipIndex, LockMode, DestStride, bLockWithinMiptail);
		RHITHREAD_GLCOMMAND_EPILOGUE_GET_RETURN(void *);
		Result = ReturnValue;
		MipBytes = ResourceCast_Unfenced(Texture)->GetLockSize(MipIndex, 0, LockMode, DestStride);
	}
	else
	{
		MipBytes = ResourceCast_Unfenced(Texture)->GetLockSize(MipIndex, 0, LockMode, DestStride);
		Result = FMemory::Malloc(MipBytes, 16);
	}
	check(Result);
	GLLockTracker.Lock(Texture, Result, ArrayIndex, MipIndex, DestStride, MipBytes, LockMode);
	return Result;
}

void FOpenGLDynamicRHI::RHIUnlockTextureCubeFace_RenderThread(class FRHICommandListImmediate& RHICmdList, FRHITextureCube* Texture, uint32 FaceIndex, uint32 ArrayIndex, uint32 MipIndex, bool bLockWithinMiptail)
{
	check(IsInRenderingThread());
	static auto* CVarRHICmdBufferWriteLocks = IConsoleManager::Get().FindTConsoleVariableDataInt(TEXT("r.RHICmdBufferWriteLocks"));
	bool bBuffer = CVarRHICmdBufferWriteLocks->GetValueOnRenderThread() > 0;
	FTextureLockTracker::FLockParams Params = GLLockTracker.Unlock(Texture, ArrayIndex, MipIndex);
	if (!bBuffer || Params.LockMode != RLM_WriteOnly || RHICmdList.Bypass() || !IsRunningRHIInSeparateThread())
	{
		GLLockTracker.TotalMemoryOutstanding = 0;
		RHITHREAD_GLCOMMAND_PROLOGUE();
		this->RHIUnlockTextureCubeFace(Texture, FaceIndex, ArrayIndex, MipIndex, bLockWithinMiptail);
		RHITHREAD_GLCOMMAND_EPILOGUE();
	}
	else
	{
		auto GLCommand = [=]()
		{
			uint32 DestStride;
			uint8* TexMem = (uint8*)this->RHILockTextureCubeFace(Texture, FaceIndex, ArrayIndex, MipIndex, RLM_WriteOnly, DestStride, bLockWithinMiptail);
			uint8* BuffMem = (uint8*)Params.Buffer;
			check(DestStride == Params.Stride);
			FMemory::Memcpy(TexMem, BuffMem, Params.BufferSize);
			FMemory::Free(Params.Buffer);
			this->RHIUnlockTextureCubeFace(Texture, FaceIndex, ArrayIndex, MipIndex, bLockWithinMiptail);
		};
		ALLOC_COMMAND_CL(RHICmdList, FRHICommandGLCommand)(MoveTemp(GLCommand));
	}
}

void* FOpenGLDynamicRHI::LockTexture2DArray_RenderThread(class FRHICommandListImmediate& RHICmdList, FRHITexture2DArray* Texture, uint32 ArrayIndex, uint32 MipIndex, EResourceLockMode LockMode, uint32& DestStride, bool bLockWithinMiptail)
{
	check(IsInRenderingThread());
	static auto* CVarRHICmdBufferWriteLocks = IConsoleManager::Get().FindTConsoleVariableDataInt(TEXT("r.RHICmdBufferWriteLocks"));
	bool bBuffer = CVarRHICmdBufferWriteLocks->GetValueOnRenderThread() > 0;
	void* Result;
	uint32 MipBytes = 0;
	if (!bBuffer || LockMode != RLM_WriteOnly || RHICmdList.Bypass() || !IsRunningRHIInSeparateThread())
	{
		RHITHREAD_GLCOMMAND_PROLOGUE();
		return this->RHILockTexture2DArray(Texture, ArrayIndex, MipIndex, LockMode, DestStride, bLockWithinMiptail);
		RHITHREAD_GLCOMMAND_EPILOGUE_GET_RETURN(void*);
		Result = ReturnValue;
		MipBytes = ResourceCast_Unfenced(Texture)->GetLockSize(MipIndex, ArrayIndex, LockMode, DestStride);
	}
	else
	{
		MipBytes = ResourceCast_Unfenced(Texture)->GetLockSize(MipIndex, ArrayIndex, LockMode, DestStride);
		Result = FMemory::Malloc(MipBytes, 16);
	}
	check(Result);

	GLLockTracker.Lock(Texture, Result, ArrayIndex, MipIndex, DestStride, MipBytes, LockMode);
	return Result;
}

void FOpenGLDynamicRHI::UnlockTexture2DArray_RenderThread(class FRHICommandListImmediate& RHICmdList, FRHITexture2DArray* Texture, uint32 ArrayIndex, uint32 MipIndex, bool bLockWithinMiptail)
{
	check(IsInRenderingThread());
	static auto* CVarRHICmdBufferWriteLocks = IConsoleManager::Get().FindTConsoleVariableDataInt(TEXT("r.RHICmdBufferWriteLocks"));
	bool bBuffer = CVarRHICmdBufferWriteLocks->GetValueOnRenderThread() > 0;
	FTextureLockTracker::FLockParams Params = GLLockTracker.Unlock(Texture, ArrayIndex, MipIndex);
	if (!bBuffer || Params.LockMode != RLM_WriteOnly || RHICmdList.Bypass() || !IsRunningRHIInSeparateThread())
	{
		GLLockTracker.TotalMemoryOutstanding = 0;
		RHITHREAD_GLCOMMAND_PROLOGUE();
		this->RHIUnlockTexture2DArray(Texture, ArrayIndex, MipIndex, bLockWithinMiptail);
		RHITHREAD_GLCOMMAND_EPILOGUE();
	}
	else
	{
		auto GLCommand = [=]()
		{
			uint32 DestStride;
			uint8* TexMem = (uint8*)this->RHILockTexture2DArray(Texture, ArrayIndex, MipIndex, Params.LockMode, DestStride, bLockWithinMiptail);
			uint8* BuffMem = (uint8*)Params.Buffer;
			check(DestStride == Params.Stride);
			FMemory::Memcpy(TexMem, BuffMem, Params.BufferSize);
			FMemory::Free(Params.Buffer);
			this->RHIUnlockTexture2DArray(Texture, ArrayIndex, MipIndex, bLockWithinMiptail);
		};
		ALLOC_COMMAND_CL(RHICmdList, FRHICommandGLCommand)(MoveTemp(GLCommand));
	}
}

void LogTextureEvictionDebugInfo()
{
	static int counter = 0;
	if (GOGLTextureEvictLogging && ++counter == 100)
	{
		UE_LOG(LogRHI, Warning, TEXT("txdbg: Texture mipmem %d. GTotalTexStorageSkipped %d, GTotalCompressedTexStorageSkipped %d, Total noncompressed = %d"), GTotalEvictedMipMemStored, GTotalTexStorageSkipped, GTotalCompressedTexStorageSkipped, GTotalTexStorageSkipped - GTotalCompressedTexStorageSkipped);
		UE_LOG(LogRHI, Warning, TEXT("txdbg: Texture GTotalEvictedMipMemDuplicated %d"), GTotalEvictedMipMemDuplicated);
	UE_LOG(LogRHI, Warning, TEXT("txdbg: Texture GTotalMipRestores %d, GTotalMipStoredCount %d"), GTotalMipRestores, GTotalMipStoredCount);
		UE_LOG(LogRHI, Warning, TEXT("txdbg: Texture GAvgRestoreTime %f (%d), GMaxRestoreTime %f"), GAvgRestoreCount ? GAvgRestoreTime / GAvgRestoreCount : 0.f, GAvgRestoreCount, GMaxRestoreTime);
		UE_LOG(LogRHI, Warning, TEXT("txdbg: Texture LRU %d"), FTextureEvictionLRU::Get().Num());

		GAvgRestoreCount = 0;
		GMaxRestoreTime = 0;
		GAvgRestoreTime = 0;

		counter = 0;
	}
}

void FTextureEvictionLRU::TickEviction()
{
#if (UE_BUILD_DEBUG || UE_BUILD_DEVELOPMENT)
	LogTextureEvictionDebugInfo();
#endif

	FScopeLock Lock(&TextureLRULock);
	FOpenGLTextureLRUContainer& TextureLRU = GetLRUContainer();
	for (int32 EvictCount = 0; 
		TextureLRU.Num() && (TextureLRU.GetLeastRecent()->EvictionParamsPtr->FrameLastRendered + GOGLTextureEvictFramesToLive) < GFrameNumberRenderThread && EvictCount < GOGLTexturesToEvictPerFrame
		;EvictCount++)
	{
		FOpenGLTextureBase* RemovedFromLRU = TextureLRU.RemoveLeastRecent();
		RemovedFromLRU->EvictionParamsPtr->LRUNode = FSetElementId();
		RemovedFromLRU->TryEvictGLResource();
	}
}

void FTextureEvictionLRU::Remove(FOpenGLTextureBase* TextureBase)
{
	if( TextureBase->EvictionParamsPtr.IsValid() )
	{
		FScopeLock Lock(&TextureLRULock);

		check(!TextureBase->EvictionParamsPtr->LRUNode.IsValidId() || GetLRUContainer().Contains(TextureBase));
		check(TextureBase->EvictionParamsPtr->LRUNode.IsValidId() || !GetLRUContainer().Contains(TextureBase));
		if( TextureBase->EvictionParamsPtr->LRUNode.IsValidId())
		{
			GetLRUContainer().Remove(TextureBase);
			TextureBase->EvictionParamsPtr->LRUNode = FSetElementId();
		}
	}
}

bool FTextureEvictionLRU::Add(FOpenGLTextureBase* TextureBase)
{
	FScopeLock Lock(&TextureLRULock); 
	check(TextureBase->EvictionParamsPtr);
	check(!TextureBase->EvictionParamsPtr->LRUNode.IsValidId())
	FOpenGLTextureLRUContainer& TextureLRU = GetLRUContainer();
	check(!TextureLRU.Contains(TextureBase));

	if(ensure(TextureLRU.Num() != TextureLRU.Max()))
	{
		TextureBase->EvictionParamsPtr->LRUNode = TextureLRU.Add(TextureBase, TextureBase);
		TextureBase->EvictionParamsPtr->FrameLastRendered = GFrameNumberRenderThread;
		return true;
	}
	return false;
}

void FTextureEvictionLRU::Touch(FOpenGLTextureBase* TextureBase)
{
	FScopeLock Lock(&TextureLRULock);
	check(TextureBase->EvictionParamsPtr);
	check(TextureBase->EvictionParamsPtr->LRUNode.IsValidId())
	check(GetLRUContainer().Contains(TextureBase));
	GetLRUContainer().MarkAsRecent(TextureBase->EvictionParamsPtr->LRUNode);
	TextureBase->EvictionParamsPtr->FrameLastRendered = GFrameNumberRenderThread;
}

FOpenGLTextureBase* FTextureEvictionLRU::GetLeastRecent()
{
	return GetLRUContainer().GetLeastRecent();
}


FTextureEvictionParams::FTextureEvictionParams(uint32 NumMips) : bHasRestored(0), FrameLastRendered(0)
{
	MipImageData.Reserve(NumMips);
	MipImageData.SetNum(NumMips);
}

FTextureEvictionParams::~FTextureEvictionParams()
{
	VERIFY_GL_SCOPE();

	if (bHasRestored)
	{
		GTotalEvictedMipMemDuplicated -= GetTotalAllocated();
	}

	for (int i = MipImageData.Num() - 1; i >= 0; i--)
	{
		GTotalEvictedMipMemStored -= MipImageData[i].Num();
	}
	GTotalMipStoredCount -= MipImageData.Num();
}

void FTextureEvictionParams::SetMipData(uint32 MipIndex, const void* Data, uint32 Bytes)
{
	checkf(Bytes, TEXT("FTextureEvictionParams::SetMipData: MipIndex %d, Data %p, Bytes %d)"), MipIndex, Data, Bytes);

	VERIFY_GL_SCOPE();
	if (MipImageData[MipIndex].Num())
	{
		// already have data??
		checkNoEntry();
	}
	else
	{
		GTotalMipStoredCount++;
	}
	MipImageData[MipIndex].Reserve(Bytes);
	MipImageData[MipIndex].SetNumUninitialized(Bytes);
	if (Data)
	{
		FMemory::Memcpy(MipImageData[MipIndex].GetData(), Data, Bytes);
	}
	GTotalEvictedMipMemStored += Bytes;
}

void FTextureEvictionParams::CloneMipData(const FTextureEvictionParams& Src, uint32 InNumMips, int32 SrcOffset, int DstOffset)
{
	VERIFY_GL_SCOPE();

	int32 MaxMip = FMath::Min((int32)InNumMips, (int32)Src.MipImageData.Num() - SrcOffset);
	for (int32 MipIndex = 0; MipIndex < MaxMip; ++MipIndex)
	{
		if (MipImageData[MipIndex + DstOffset].Num())
		{
			checkNoEntry();
		}
		else
		{
			GTotalMipStoredCount++;
		}
		MipImageData[MipIndex + DstOffset] = Src.MipImageData[MipIndex + SrcOffset];
		GTotalEvictedMipMemStored += MipImageData[MipIndex + DstOffset].Num();
	}
}

void FTextureEvictionParams::ReleaseMipData(uint32 RetainMips)
{
	VERIFY_GL_SCOPE();

	for (int i = MipImageData.Num() - 1 - RetainMips; i >= 0; i--)
	{
		GTotalEvictedMipMemStored -= MipImageData[i].Num();
		GTotalMipStoredCount -= MipImageData[i].Num() ? 1 : 0;
		MipImageData[i].Empty();
	}

	// if we're retaining mips then keep entire MipImageData array to ensure there's no MipIndex confusion.
	if (RetainMips == 0)
	{
		MipImageData.Empty();
	}
}
