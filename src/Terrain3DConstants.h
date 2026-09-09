// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_CONSTANTS_H
#define TERRAIN3D_CONSTANTS_H

// Low-level constants required by the EASTL allocator dependency chain.
#define s_nRuntimeAllocationPhaseCount ( 6u )
#define s_nRouteGateClosed ( UINT32_C( 0x80000000 ) )
#define s_nRouteReaderMask ( ~s_nRouteGateClosed )

#include "core/terrain_3d_compiler.h"

#include <float.h>
#include <stddef.h>
#include <stdint.h>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/geometry_instance3d.hpp>
#include <godot_cpp/classes/physics_server3d.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <godot_cpp/variant/vector2.hpp>
#include <godot_cpp/variant/vector2i.hpp>
#include <godot_cpp/variant/vector3.hpp>

// GDExtension uses the godot namespace, custom modules do not.
#if defined( GDEXTENSION ) && !defined( GODOT_MODULE )
using namespace godot;
#endif

// Centralized project constants. Kept as #define values for the target-era style.
#define s_nProfileVersion ( 5u )
#define s_nProfileFnvOffset ( 1469598103934665603ull )
#define s_nProfileFnvPrime ( 1099511628211ull )
#define s_nHashGoldenRatio32 ( 0x9e3779b9u )
#define s_nHashMix32 ( 0x85ebca6bu )
#define s_nHashMix32Secondary ( 0xc2b2ae35u )
#define s_nHashGoldenRatio64 ( 0x9e3779b97f4a7c15ULL )
#define s_fUint32MaxFloat ( 4294967295.0f )
#define s_nTestHalfEncodingCount ( 65536u )
#define s_nTestTransformCount ( 37u )
#define s_nTestNormalWidth ( 37 )
#define s_nTestNormalHeight ( 19 )
#define s_nTestNormalStride ( s_nTestNormalWidth + 2 )
#define s_nTestErosionCount ( 4099u )
#define s_nTestNonFiniteErosionCount ( 16u )
#define s_nTestDispatchCount ( 1024u )
#define s_nTestDispatchRounds ( 32u )
#define s_nTestDispatchIterations ( 64u )
#define s_nTestEastlAlignment ( 64u )
#define s_nTestEastlOffset ( 13u )
#define s_nTestControlFlag ( 1u << 0u )
#define s_nMemorySubsystemNameCount ( sizeof( s_pszMemorySubsystemNames ) / sizeof( s_pszMemorySubsystemNames[0] ) )
#define s_shadowCastingOn ( GeometryInstance3D::SHADOW_CASTING_SETTING_ON )
#define s_shadowCastingOff ( GeometryInstance3D::SHADOW_CASTING_SETTING_OFF )
#define s_shadowCastingOnly ( GeometryInstance3D::SHADOW_CASTING_SETTING_SHADOWS_ONLY )
#define s_nMaxTextureAssets ( 32 )
#define s_nMaxMeshAssets ( 256 )
#define s_fDataCurrentVersion ( 0.95f )
#define s_fRegionHeightMin ( -384.0f )
#define s_fRegionHeightMax ( 768.0f )
#define s_fRegionHeightRange ( s_fRegionHeightMax - s_fRegionHeightMin )
#define s_fRegionHeightInvRange ( 1.0f / s_fRegionHeightRange )
#define s_fRegionHeightU16Scale ( 65535.0f )
#define s_nRegionDefaultControl ( 1u )
#define s_nAutoshaderMaterialMask ( ( 0x1Fu << 27u ) | ( 0x1Fu << 22u ) | ( 0xFFu << 14u ) )
#define s_fDegreesPerRadian ( 57.29577951308232f )
#define s_fSqrtTwo ( 1.4142135623730951f )
#define s_fLargeDistance ( 1.0e20f )
#define s_nLandformMaterialMask ( s_nAutoshaderMaterialMask | 0x1u )
#define s_fLandformEpsilon ( 0.0001f )
#define s_nErosionDirectionCount ( 4 )
static const int32_t g_t3dConstStorage_s_szErosionDirectionX[s_nErosionDirectionCount] = { -1, 1, 0, 0 };
#define s_szErosionDirectionX g_t3dConstStorage_s_szErosionDirectionX
static const int32_t g_t3dConstStorage_s_szErosionDirectionY[s_nErosionDirectionCount] = { 0, 0, -1, 1 };
#define s_szErosionDirectionY g_t3dConstStorage_s_szErosionDirectionY
static const int32_t g_t3dConstStorage_s_szErosionOppositeDirection[s_nErosionDirectionCount] = { 1, 0, 3, 2 };
#define s_szErosionOppositeDirection g_t3dConstStorage_s_szErosionOppositeDirection
#define s_fErosionEpsilon ( 1.0e-6f )
#define s_nErosionFnvOffset ( 1469598103934665603ULL )
#define s_nErosionFnvPrime ( 1099511628211ULL )
#define s_nErosionHalfDecodeBlock ( 256u )
#define s_fClipmapPartStatic ( 0.0f )
#define s_fClipmapPartEdgeA0 ( 1.0f )
#define s_fClipmapPartEdgeA1 ( 2.0f )
#define s_fClipmapPartEdgeB0 ( 3.0f )
#define s_fClipmapPartEdgeB1 ( 4.0f )
#define s_nMultimeshStrideFloats ( 16 )
#define s_nComputeWorkgroupSize ( 64u )
#define s_nShaderMaxConditionalDepth ( 32 )
#define s_nInstancePageMagic ( 0x49334454u )
#define s_nInstancePageVersion ( 2u )
#define s_nInstancePageLegacyVersion ( 1u )
#define s_nInstancePageHeaderBytes ( 16u )
#define s_nInstancePageMeshHeaderBytes ( 12u )
#define s_nInstancePageLegacyMeshHeaderBytes ( 8u )
#define s_nInstancePageCellHeaderBytes ( 12u )
#define s_nInstancePageCellColors ( 1u << 0u )
#define s_nInstancePageCellOwners ( 1u << 1u )
#define s_nInstancePageCellFlags ( s_nInstancePageCellColors | s_nInstancePageCellOwners )
#define s_nActiveAccessReaderMask ( 0x7fffffffu )
#define s_nActiveAccessWriterBit ( 0x80000000u )
#define s_nActiveStorageAlignment ( 64u )
#define s_nAutoshaderModeLegacyRuntime ( 0 )
#define s_nAutoshaderModeBaked ( 1 )
#define s_nAutoshaderModeDisabled ( 2 )
#define s_nAutoshaderModeEditorPreview ( 3 )
#define s_nBakeCachelineBytes ( 64u )
#define s_nComputeScratchRetainLimit ( 32u * 1024u * 1024u )
#define s_nCpuCacheBytesPerTexel ( 12u )
#define s_nCpuCacheMaxLayers ( 64 )
#define s_nCpuCacheMib ( 1024u * 1024u )
#define s_nCpuCacheMinSampleLayers ( 4 )
#define s_nCpuRegionCacheMaxBudgetMb ( 128 )
#define s_nCpuRegionCacheMinBudgetMb ( 1 )
#define s_nCpuRegionLoadMaxBudgetMb ( 64 )
#define s_nCpuRegionLoadMaxLayers ( 64 )
#define s_nDefaultResidentPoolBudget ( 64u * 1024u * 1024u )
#define s_nDiagnosticsVersion ( 1 )
#define s_nDirectionalReceiverLayer ( 1u << 19u )
#define s_nErosionTextureProfileCount ( 32 )
#define s_nFeatureAltValue ( 1u << 0u )
#define s_nFeatureAngleValue ( 1u << 3u )
#define s_nFeatureCenterValidValue ( 1u << 1u )
#define s_nFeatureScaleValue ( 1u << 4u )
#define s_nFeatureTextureValue ( 1u << 2u )
#define s_nGpuHeightCacheMaxLayers ( 512 )
#define s_nGpuHeightCacheMinSpareLayers ( 8 )
#define s_nGpuFarAtlasHeaderBytes ( 52u )
#define s_nGpuFarAtlasMagic ( 0x5444334641523031ull )
#define s_nGpuFarAtlasVersion ( 9u )
#define s_nGpuRegionCacheMaxBudgetMb ( 512 )
#define s_nGpuRegionCacheMinBudgetMb ( 1 )
#define s_nGpuRegionUploadMaxBudgetMb ( 64 )
#define s_nGpuRegionUploadMaxLayers ( 64 )
#define s_nInstanceDefaultCellSize ( 32 )
#define s_nInstanceMaxCellsPerAxis ( 16 )
#define s_nInstanceMaxCellSize ( 128 )
#define s_nInstanceMaxRegionSize ( s_nRegionSize )
#define s_nInstanceMinCellSize ( 8 )
#define s_nInstanceMinRegionSize ( s_nRegionSize )
#define s_nInstanceRegionCacheMaxBudgetMb ( 128 )
#define s_nInstanceRegionCacheMinBudgetMb ( 1 )
#define s_nInstanceRegionLoadMaxBudgetMb ( 64 )
#define s_nInstanceRegionLoadMaxPages ( 64 )
#define s_nInstanceTransitionBucketCellSpan ( 4 )
#define s_nInstanceWhiteRgba8 ( 0xffffffffu )
#define s_nLightingLayerFoliage ( 1u << 15u )
#define s_nLightingLayerLarge ( 1u << 17u )
#define s_nLightingLayerMedium ( 1u << 16u )
#define s_nLightingLayerRegional ( 1u << 18u )
#define s_nLightmapActiveInstanceLimit ( 8 )
#define s_nLightmapFnvOffset ( 1469598103934665603ull )
#define s_nLightmapFnvPrime ( 1099511628211ull )
#define s_nMaxClipmapLevels ( 16 )
#define s_nMaxRegionBlendSamples ( s_nRegionSize )
#define s_nMaxCollisionAxisSamples ( s_nMaxRegionBlendSamples + 1 )
#define s_nMaxCollisionShapeRecords ( 4096 )
#define s_nMaxRenderBatches ( 5 )
#define s_nMaxShaderVariantCacheLimit ( 256 )
#define s_nMaxShaderVariantPrewarmLimit ( 64 )
#define s_nMaxShaderVariantSourceBudgetMb ( 256 )
#define s_nMaxTileInstances ( 16 )
#define s_nMemoryMib ( 1024ULL * 1024ULL )
#define s_nMemoryPolicyIntervalFrames ( 30 )
#define s_nMeshMaxLodCount ( 10 )
#define s_nMeshShadowLodId ( -1 )
#define s_nMinShaderVariantCacheLimit ( 8 )
#define s_nMinShaderVariantPrewarmLimit ( 1 )
#define s_nMinShaderVariantSourceBudgetMb ( 1 )
#define s_nOpAddValue ( 0u )
#define s_nOpGradientValue ( 4u )
#define s_nOpReplaceValue ( 2u )
#define s_nOpSubtractValue ( 1u )
#define s_nPairInstances ( 2 )
#define s_nRenderControlBlendRadius ( 2 )
static const int g_t3dConstStorage_s_szRenderControlKernel[5] = { 1, 2, 3, 2, 1 };
#define s_szRenderControlKernel g_t3dConstStorage_s_szRenderControlKernel
#define s_nRenderControlMaterialMask ( ( 0x1Fu << 27u ) | ( 0x1Fu << 22u ) | ( 0xFFu << 14u ) )
#define s_nResidentCacheAlignment ( 64u )
#define s_nResidentInvalidOffset ( ( size_t )( -1 ) )
#define s_nRingTileInstances ( 12 )
#define s_nRuntimeProfileHistoryCapacity ( 600u )
#define s_nSamplerCacheSize ( 8 )
#define s_nToolAutoshaderValue ( 6u )
#define s_nToolColorValue ( 4u )
#define s_nToolHeightValue ( 2u )
#define s_nToolHolesValue ( 7u )
#define s_nToolNavigationValue ( 8u )
#define s_nToolRoughnessValue ( 5u )
#define s_nToolTextureValue ( 3u )
#define s_nTypeColorValue ( 2u )
#define s_nTypeControlValue ( 1u )
#define s_nTypeHeightValue ( 0u )
#define s_nTypeMaxValue ( 3u )
#define s_nWorldTexels ( 4096 )
#define s_nRegionSize ( 128 )
#define s_nRegionVertexSpacingMeters ( 1u )
#define s_fRegionVertexSpacing ( 1.0f )
#define s_nRegionWorldSizeMeters ( s_nRegionSize * s_nRegionVertexSpacingMeters )
#define s_nRegionShift ( 7 )
#define s_nRegionMask ( 127 )
#define s_nRegionMapSize ( 32 )
#define s_nRegionMapShift ( 5 )
#define s_nRegionMapMask ( 31 )
#define s_nRegionSlots ( 1024 )
#define s_nRegionMapHalf ( 16 )
#define s_nRegionTexels ( 16384 )
#define s_nRegionHeightBytes ( 32768 )
#define s_nRegionGeometrySignature                                                                                                        \
	( 0x80000000u | ( ( uint32_t )( s_nRegionSize ) << 16u ) | ( ( uint32_t )( s_nRegionVertexSpacingMeters ) << 8u )                    \
	        | ( uint32_t )( s_nRegionMapSize ) )
#define s_nXbox360CpuMapCeilingBytes ( 64u * 1024u * 1024u )
#define s_nActiveWorldTexels ( s_nWorldTexels )
#define s_nBakeUvCachelineElements ( s_nBakeCachelineBytes / sizeof( Vector2 ) )
#define s_nBakeVertexCachelineElements ( s_nBakeCachelineBytes / ( sizeof( Vector3 ) / 3u ) )
#define s_nGpuCacheFarColorBytes ( ( size_t )( s_nGpuCacheFarHeightSide ) * ( size_t )( s_nGpuCacheFarHeightSide ) * 4u )
#define s_nGpuCacheFarHeightBytes ( ( size_t )( s_nGpuCacheFarHeightSide ) * ( size_t )( s_nGpuCacheFarHeightSide ) * 2u )
#define s_nGpuFarHeightSize ( s_nGpuCacheFarHeightSide )
#define s_nLightingClassLayerMask ( s_nLightingLayerFoliage | s_nLightingLayerMedium | s_nLightingLayerLarge | s_nLightingLayerRegional )
#define s_nMaxBatchInstances ( s_nMaxClipmapLevels )
#define s_nMaxBufferShaderVariantCacheLimit ( s_nMaxShaderVariantCacheLimit / 2 )
#define s_nMaxEditorShaderVariantCacheLimit ( s_nMaxShaderVariantCacheLimit / 4 )
#define s_nMemoryPolicyHysteresisBytes ( 2ULL * s_nMemoryMib )
#define s_nBakeTaskAlignmentElements ( s_nBakeVertexCachelineElements )
#define s_nLightingManagedLayerMask ( s_nLightingClassLayerMask | s_nDirectionalReceiverLayer )
#define s_nResidentWorldSize ( s_nRegionMapSize )
#define s_nInstanceRegionTableCapacity ( ( size_t )( s_nRegionSlots ) )
#define s_nResidentWorldHalf ( s_nResidentWorldSize / 2 )
#define s_nResidentWorldSlotCount ( s_nRegionSlots )
#define s_nInstanceRegionUpdateCapacity ( s_nInstanceRegionTableCapacity )
#define s_fControlAutoFloat ( FLT_TRUE_MIN )
#define s_nRuntimeMaterialMagic ( 0x5433444D41543031ull ) // T3DMAT01
#define s_nRuntimeMaterialVersion ( 4u )
#define s_nRuntimeMaterialSpareLayers ( 2 )
#define s_nRuntimeMaterialHeaderBytes ( 32u )
#define s_nRuntimeMaterialImageDescriptorBytes ( 32u )
#define s_nRuntimeMaterialCookedChannels ( 5u )
#define s_nRuntimeMaterialFixedSettingsBytes ( 60u )
#define s_nRuntimeMaterialLayerDescriptorBytes                                                                                             \
	( s_nRuntimeMaterialCookedChannels * s_nRuntimeMaterialImageDescriptorBytes + 16u + s_nRuntimeMaterialFixedSettingsBytes )
#define s_nRuntimeMaterialDescriptorTableBytes ( s_nMaxTextureAssets * s_nRuntimeMaterialLayerDescriptorBytes )
#define s_nRuntimeMaterialDataOffset ( s_nRuntimeMaterialHeaderBytes + s_nRuntimeMaterialDescriptorTableBytes )
#define MAX_TEXTURE_ASSETS ( s_nMaxTextureAssets )
#define MAX_MESH_ASSETS ( s_nMaxMeshAssets )
#define s_nCookDatabaseMagic ( 0x3142444b43443354ull ) // T3DCKDB1
#define s_nCookDatabaseVersion ( 2u )
#define s_nCookDatabaseHeaderBytes ( 40u )
#define s_fByteMaxFloat ( 255.0f )
#define s_fByteMidpointFloat ( 127.5f )
#define s_fByteInvMaxFloat ( 1.0f / s_fByteMaxFloat )
#define T3D_UNIT_TESTING_PUBLICATION_COUNT ( 10000u )
#define T3D_UNIT_TESTING_CHECK_MASK ( 0x9e3779b97f4a7c15ULL )
#define T3D_UNIT_TESTING_COMMAND_BUFFER_BYTES ( sizeof( t3dRdIndexedIndirectCommand_t ) * s_nRdDrawGroupCount )
#define s_nComputeBatchSize ( 3u )
#define s_nHoleMagic ( 0x314c4f48u )            // HOL1
#define s_nNavMagic ( 0x3156414eu )             // NAV1
#define s_nMinmaxMagic ( 0x31584d4du )          // MMX1
#define s_nCollisionHeightMagic ( 0x31484c43u ) // CLH1
#define s_nCollisionBoundsMagic ( 0x31424c43u ) // CLB1
#define s_nNodeMagic ( 0x31444f4eu )            // NOD1
#define s_nInstanceMetaMagic ( 0x314d4e49u )    // INM1
#define s_nLightingMagic ( 0x3154474cu )        // LGT1
#define s_nMultimeshMagic ( 0x31494d4du )       // MMI1
#define s_nBlobVersion ( 2u )
#define s_nFixed ( 8u + 2u + 2u + 4u + 2u + 2u + 16u + 2u + 2u + 4u + 48u + 4u )
#define s_nRuntimePageAlignment ( 16u )
#define s_nRuntimePageBlendRecipeVersion ( 4u )
#define s_nRuntimePageChannelCount ( 24u )
#define s_nRuntimePageHeaderBytes ( 1248u )
#define s_nRuntimePageMagic ( 0x47503354u )
#define s_nRuntimePageMaxBytes ( 256u * 1024u * 1024u )
#define s_nRuntimePageVersion ( 7u )
#define s_nManifestHeaderBytes ( 112u )
#define s_nManifestTopologyBytes ( s_nRegionSlots / 8u )
#define s_nManifestRegionBaseBytes ( 112u )
#define s_nRuntimeWorldManifestMagic ( 0x31464e4d57334454ull ) // T3DWMNF1
#define s_nRuntimeWorldManifestVersion ( 5u )
#define s_nRuntimeWorldAdjN ( 1u << 0u )
#define s_nRuntimeWorldAdjNe ( 1u << 1u )
#define s_nRuntimeWorldAdjE ( 1u << 2u )
#define s_nRuntimeWorldAdjSe ( 1u << 3u )
#define s_nRuntimeWorldAdjS ( 1u << 4u )
#define s_nRuntimeWorldAdjSw ( 1u << 5u )
#define s_nRuntimeWorldAdjW ( 1u << 6u )
#define s_nRuntimeWorldAdjNw ( 1u << 7u )
#define s_nHlodRuntimeMagic ( 0x32444c48u ) // HLD2
#define s_nHlodRuntimeVersion ( 2u )
#define s_nInstanceCacheMaxPages ( 64 )
#define s_nInstanceCacheMib ( 1024u * 1024u )
#define s_nActivePhysicalPageCount ( 32 )
#define s_nActiveInvalidPage ( -1 )
#define s_nArenaCachelineAlignment ( 64u )
#define s_nArenaDefaultBaseAlignment ( 64u )
#define s_nRuntimeAllocatorBudgetDivisor ( 8u )
#define s_nRuntimeAllocatorMinBytes ( 4u * 1024u * 1024u )
#define s_nRuntimeAllocatorMaxBytes ( 64u * 1024u * 1024u )
#define s_nSmallClassCount ( 9u )
#define s_nSmallPageBytes ( 64u * 1024u )
#define s_nMaxExtents ( 8192u )
#define s_nMaxSmallPages ( 1024u )
#define s_nAllocationMagic ( 0x5A4F4E453344544FULL )
#define s_nResidentSlotFree ( 0u )
#define s_nResidentSlotStaging ( 1u )
#define s_nResidentSlotPublished ( 2u )
#define s_nResidentSlotRetiring ( 3u )
#define s_nAcquireGateClosed ( UINT32_C( 0x80000000 ) )
#define s_nAcquireReaderMask ( ~s_nAcquireGateClosed )
#define s_nBackingBytes ( 512u * 1024u )
#define s_nCollisionBakedCacheAxis ( 8 )
#define s_nBakedCacheCapacity ( s_nCollisionBakedCacheAxis * s_nCollisionBakedCacheAxis )
#define s_nDynamicScratchAxis ( 65 )
#define s_nDynamicScratchSamples ( s_nDynamicScratchAxis * s_nDynamicScratchAxis )
#define s_nRegionPresenceSamples ( 25 )
#define T3D_TERRAIN_3D_RD_CLIPMAP_RENDERER_INDIRECT_BUFFER_BYTES ( sizeof( t3dRdIndexedIndirectCommand_t ) * s_nRdDrawGroupCount )
#define s_nMaxViews ( 4u )
#define s_nForwardPipelineCapacity ( 64u )
#define s_nResourceCapacity ( 1024u )
#define s_nResourceHistoryCapacity ( 1024u )
#define s_nFreedHistoryCapacity ( 256u )
#define s_nVertexFormatCapacity ( 64u )
#define T3D_TERRAIN_3D_RD_CULLING_COMMAND_BYTES ( sizeof( t3dRdIndexedIndirectCommand_t ) )
#define T3D_TERRAIN_3D_RD_CULLING_BUFFER_BYTES ( sizeof( t3dRdIndexedIndirectCommand_t ) * s_nRdDrawGroupCount )
#define s_nMaxSets ( 32u )
#define s_nT3dForwardBridgeAbi ( 11u )
#define s_nT3dForwardBridgeMaxViews ( 2u )
#define s_nT3dForwardBridgeRequiredCapabilities                                                                                            \
	( T3D_RD_FORWARD_CAP_DIRECT_POD | T3D_RD_FORWARD_CAP_DEPTH | T3D_RD_FORWARD_CAP_OPAQUE | T3D_RD_FORWARD_CAP_SHADOW                     \
	        | T3D_RD_FORWARD_CAP_MOTION | T3D_RD_FORWARD_CAP_TRANSPARENT | T3D_RD_FORWARD_CAP_CLUSTERED_LIGHTING                           \
	        | T3D_RD_FORWARD_CAP_DECALS | T3D_RD_FORWARD_CAP_REFLECTIONS | T3D_RD_FORWARD_CAP_RESOURCE_GENERATIONS                         \
	        | T3D_RD_FORWARD_CAP_SPLIT_SHADOW_PACKET | T3D_RD_FORWARD_CAP_ENVIRONMENT_LIGHTING )
#define s_nBridgeAbiVersion ( s_nT3dForwardBridgeAbi )
#define s_nMaxCoordinators ( 64u )
#define s_nDirtyChunkRecords ( 64u )
#define s_nFnvOffset ( 14695981039346656037ull )
#define s_nFnvPrime ( 1099511628211ull )
#define s_nMinVertexArenaBytes ( 4u * 1024u * 1024u )
#define s_nMinIndexArenaBytes ( 2u * 1024u * 1024u )
#define T3D_TERRAIN_3D_RD_MESH_CACHE_CHUNK_BYTES ( 65536u )
#define s_nDirtyUploadChunkBytes ( 65536u )
#define s_nPipelineCapacity ( 32u )
#define T3D_RD_RW_FORWARD_COLOR_VARIANT_COUNT ( 8u )
#define s_nTrackedBatchSlotCount ( 256u )
#define s_nTrackedBatchUpdateCount ( 6u )
#define s_nFnv1aOffset ( 14695981039346656037ull )
#define s_nFnv1aPrime ( 1099511628211ull )
#define s_fPackedIntegerTolerance ( 0.000001 )
#define s_nRdMaterialLayerCount ( 32u )
#define s_nRdDrawGroupCount ( 4u )
#define s_nRdFrameSnapshotSlotCount ( 3u )
#define s_nRdInvalidFrameSnapshotIndex ( s_nRdFrameSnapshotSlotCount )
#define s_nRdFrameSnapshotTokenIndexMask ( 0x3u )
#define s_nRdFrameSnapshotTokenPublishedMask ( uint64_t( 1u ) << 63u )
#define s_nRdFrameSnapshotGenerationMax ( ( uint64_t( 1u ) << 61u ) - 1u )
#define s_nRdForwardBridgeAbiVersion ( 10u )
#define s_nRdForwardMaxViews ( 2u )
#define s_nEditorBrushTextureSize ( 256 )
#define s_nForwardViewportSlotCount ( 8u )
#define s_nDerivedReadbackSlotCount ( 3u )
#define s_nShaderPackMagic ( 0x314b504853443354ull ) // T3DSHPK1
#define s_nShaderPackVersion ( 1u )
#define s_nClipmapPackMagic ( 0x314b504d504c4333ull ) // 3CLMPPK1
#define s_nClipmapPackVersion ( 1u )
#define s_nClipmapWeldPartCount ( 5u )
#define s_nCpuRequestCapacity ( 25 )
#define s_nActiveRadius ( 2 )
#define s_nGpuCacheDefaultGuardRegions ( 1 )
#define s_nGpuCacheDefaultHysteresisReserve ( 4 )
#define s_nGpuCacheDefaultHysteresisTicks ( 30 )
#define s_nGpuCacheDefaultPrefetchRows ( 1 )
#define s_nGpuCacheDefaultRetireFrames ( 4 )
#define s_nGpuCacheDefaultSpareLayers ( 12 )
#define s_nGpuCacheFarHeightSide ( 512 )
#define s_nGpuCacheFarSurfaceMaxSide ( 4096 )
#define s_nGpuCacheRuntimeMaxLayers ( 256 )
#define s_nGpuCacheEditorMaxLayers ( 256 )
#define s_nGpuCacheMaxLayers ( s_nGpuCacheEditorMaxLayers )
#define s_nGpuCacheMaxPageTableBytes ( 64u * 64u * 3u )
#define s_nGpuCacheMib ( 1024u * 1024u )
#define s_nGpuCacheMinSpareLayers ( 2 )
#define s_nDiscoveryBytes ( 12u )
#define s_nRuntimeWorkerLimit ( 32 )
#define T3D_TERRAIN_3D_STREAMING_INTEGRATION_BOUNDARY_FRACTION ( 0.25 )
#define s_nNativeApiRevision ( 2026090601LL )

// File Paths (e.g. extras)
#define OCEAN_MATERIAL_PATH "res://addons/terrain_3d/extras/shaders/M_ocean.tres"

// Engine Shortcuts
#define PS PhysicsServer3D::get_singleton()
#define IS_EDITOR Engine::get_singleton()->is_editor_hint()

// Constants

// Centralized native compile-time constants. Keep tunable limits and format constants here.


// Full-region heightmap collision samples region_size + 1 vertices. Keep the
// axis scratch large enough for the maximum supported 2048-texel region.


static const Vector2i s_regionMapVSize{ s_nRegionMapSize, s_nRegionMapSize };


typedef enum t3dPerformanceProfileOwner_e : uint8_t {
	T3D_PROFILE_OWNER_TERRAIN,
	T3D_PROFILE_OWNER_MATERIAL,
} t3dPerformanceProfileOwner_t;

typedef enum t3dPerformanceProfileValueKind_e : uint8_t {
	T3D_PROFILE_VALUE_INT,
	T3D_PROFILE_VALUE_FLOAT,
	T3D_PROFILE_VALUE_BOOL,
} t3dPerformanceProfileValueKind_t;

typedef struct t3dPerformanceProfileSetting_s {
	t3dPerformanceProfileOwner_t m_owner;
	t3dPerformanceProfileValueKind_t m_kind;
	const char *m_pszProperty;
	int64_t m_nIntValue;
	double m_fFloatValue;
	bool m_bBoolValue;
} t3dPerformanceProfileSetting_t;

#define T3D_PROFILE_TERRAIN_INT( name, value ) { T3D_PROFILE_OWNER_TERRAIN, T3D_PROFILE_VALUE_INT, name, ( int64_t )( value ), 0.0, false },
#define T3D_PROFILE_TERRAIN_FLOAT( name, value )                                                                                           \
	{ T3D_PROFILE_OWNER_TERRAIN, T3D_PROFILE_VALUE_FLOAT, name, 0, ( double )( value ), false },
#define T3D_PROFILE_TERRAIN_BOOL( name, value ) { T3D_PROFILE_OWNER_TERRAIN, T3D_PROFILE_VALUE_BOOL, name, 0, 0.0, ( bool )( value ) },
#define T3D_PROFILE_MATERIAL_INT( name, value )                                                                                            \
	{ T3D_PROFILE_OWNER_MATERIAL, T3D_PROFILE_VALUE_INT, name, ( int64_t )( value ), 0.0, false },
#define T3D_PROFILE_MATERIAL_FLOAT( name, value )                                                                                          \
	{ T3D_PROFILE_OWNER_MATERIAL, T3D_PROFILE_VALUE_FLOAT, name, 0, ( double )( value ), false },
#define T3D_PROFILE_MATERIAL_BOOL( name, value ) { T3D_PROFILE_OWNER_MATERIAL, T3D_PROFILE_VALUE_BOOL, name, 0, 0.0, ( bool )( value ) },


static const t3dPerformanceProfileSetting_t g_t3dConstStorage_g_szXbox360ExperimentalSettings[] = {
#include "streaming/terrain_3d_performance_profile_manifest.inc"
};
#define g_szXbox360ExperimentalSettings g_t3dConstStorage_g_szXbox360ExperimentalSettings
#define g_nXbox360ExperimentalSettingCount ( sizeof( g_szXbox360ExperimentalSettings ) / sizeof( g_szXbox360ExperimentalSettings[0] ) )

#undef T3D_PROFILE_TERRAIN_INT
#undef T3D_PROFILE_TERRAIN_FLOAT
#undef T3D_PROFILE_TERRAIN_BOOL
#undef T3D_PROFILE_MATERIAL_INT
#undef T3D_PROFILE_MATERIAL_FLOAT
#undef T3D_PROFILE_MATERIAL_BOOL

// End centralized native compile-time constants.

static const Color s_colorNan{ NAN, NAN, NAN, NAN };
static const Color s_colorWhite{ 1.0f, 1.0f, 1.0f, 1.0f };
static const Image::Format s_szRegionMapFormats[s_nTypeMaxValue + 1u] = {
	Image::FORMAT_R16,
	Image::FORMAT_RF,
	Image::FORMAT_RGBA8,
	Image::Format( s_nTypeMaxValue ),
};

#define COLOR_BLACK Color( 0.0f, 0.0f, 0.0f, 1.0f )
#define COLOR_ROUGHNESS Color( 1.0f, 1.0f, 1.0f, 0.5f )
#define COLOR_CHECKED Color( 1.f, 1.f, 1.0f, -1.0f )
#define COLOR_NORMAL Color( 0.5f, 0.5f, 1.0f, 1.0f )

#define COLOR_CONTROL Color( s_fControlAutoFloat, 0.f, 0.f, 1.0f )
static const Color s_szRegionMapDefaultColors[s_nTypeMaxValue + 1u] = {
	COLOR_BLACK,
	COLOR_CONTROL,
	COLOR_ROUGHNESS,
	s_colorNan,
};

// For consistency between MSVC, gcc, clang
#ifndef FLT_MAX
#define FLT_MAX __FLT_MAX__
#endif

// Warning flags are stored in an 8-bit mask.
#define WARN_MISMATCHED_SIZE 0x01
#define WARN_MISMATCHED_FORMAT 0x02
#define WARN_MISMATCHED_MIPMAPS 0x04
#define WARN_ALL 0xFF

// Global Types

#define V2( x ) Vector2( x, x )
#define V2I( x ) Vector2i( x, x )
#define V3( x ) Vector3( x, x, x )
static const Vector2 s_v2Zero{ 0.f, 0.f };
static const Vector2 s_v2Max{ FLT_MAX, FLT_MAX };
static const Vector2i s_v2iZero{ 0, 0 };
static const Vector2i s_v2iMax{ INT32_MAX, INT32_MAX };

typedef struct t3dRegionCoord_s {
	int8_t m_x;
	int8_t m_y;
} t3dRegionCoord_t;

#define s_nRegionCoordInvalid ( INT8_MAX )

EA_FORCE_INLINE t3dRegionCoord_t t3dPackRegionCoord( const Vector2i &rLocation ) {
	t3dRegionCoord_t coord;
	if ( rLocation.x < -s_nRegionMapHalf || rLocation.x >= s_nRegionMapHalf || rLocation.y < -s_nRegionMapHalf
	        || rLocation.y >= s_nRegionMapHalf ) {
		coord.m_x = s_nRegionCoordInvalid;
		coord.m_y = s_nRegionCoordInvalid;
		return coord;
	}
	coord.m_x = ( int8_t )( rLocation.x );
	coord.m_y = ( int8_t )( rLocation.y );
	return coord;
}

EA_FORCE_INLINE t3dRegionCoord_t t3dInvalidRegionCoord( void ) {
	t3dRegionCoord_t coord;
	coord.m_x = s_nRegionCoordInvalid;
	coord.m_y = s_nRegionCoordInvalid;
	return coord;
}

EA_FORCE_INLINE bool t3dRegionCoordValid( const t3dRegionCoord_t &rCoord ) {
	return rCoord.m_x != s_nRegionCoordInvalid && rCoord.m_y != s_nRegionCoordInvalid;
}

EA_FORCE_INLINE Vector2i t3dUnpackRegionCoord( const t3dRegionCoord_t &rCoord ) {
	return t3dRegionCoordValid( rCoord ) ? Vector2i( ( int )( rCoord.m_x ), ( int )( rCoord.m_y ) ) : s_v2iMax;
}

EA_FORCE_INLINE bool t3dRegionCoordEquals( const t3dRegionCoord_t &rCoord, const Vector2i &rLocation ) {
	return t3dRegionCoordValid( rCoord ) && ( int )( rCoord.m_x ) == rLocation.x && ( int )( rCoord.m_y ) == rLocation.y;
}

EA_FORCE_INLINE bool t3dRegionCoordEquals( const t3dRegionCoord_t &rA, const t3dRegionCoord_t &rB ) {
	return rA.m_x == rB.m_x && rA.m_y == rB.m_y;
}

static_assert( sizeof( t3dRegionCoord_t ) == 2u, "Packed Terrain3D region coordinates must remain two bytes" );
static const Vector3 s_v3Zero{ 0.f, 0.f, 0.f };
static const Vector3 s_v3Max{ FLT_MAX, FLT_MAX, FLT_MAX };
static const Vector3 s_v3Nan{ NAN, NAN, NAN };
static const Vector3 s_v3Up{ 0.f, 1.f, 0.f };
// static const Vector3 V3_DOWN{ 0.f, -1.f, 0.f };


// Set class name for logger.h

#define CLASS_NAME() const String __class__ = get_class_static() + String( "#" ) + String::num_uint64( get_instance_id() ).right( 4 );

#define CLASS_NAME_STATIC( pszName ) static inline const char *__class__ = pszName;

// Validation macros

#define ASSERT( cond, ret )                                                                                                                \
	if ( !( cond ) ) {                                                                                                                     \
		UtilityFunctions::push_error( "Assertion '", #cond, "' failed at ", __FILE__, ":", __LINE__ );                                     \
		return ret;                                                                                                                        \
	}

#define VOID // a return value for void, to avoid compiler warnings

#define IS_INIT( ret )                                                                                                                     \
	if ( !m_pTerrain ) {                                                                                                                   \
		return ret;                                                                                                                        \
	}

#define IS_INIT_MESG( mesg, ret )                                                                                                          \
	if ( !m_pTerrain ) {                                                                                                                   \
		LOG( ERROR, mesg );                                                                                                                \
		return ret;                                                                                                                        \
	}

#define IS_INIT_COND( cond, ret )                                                                                                          \
	if ( !m_pTerrain || cond ) {                                                                                                           \
		return ret;                                                                                                                        \
	}

#define IS_INIT_COND_MESG( cond, mesg, ret )                                                                                               \
	if ( !m_pTerrain || cond ) {                                                                                                           \
		LOG( ERROR, mesg );                                                                                                                \
		return ret;                                                                                                                        \
	}

#define IS_INSTANCER_INIT( ret )                                                                                                           \
	if ( !m_pTerrain || !m_pTerrain->getInstancer() ) {                                                                                    \
		return ret;                                                                                                                        \
	}

#define IS_INSTANCER_INIT_MESG( mesg, ret )                                                                                                \
	if ( !m_pTerrain || !m_pTerrain->getInstancer() ) {                                                                                    \
		LOG( ERROR, mesg );                                                                                                                \
		return ret;                                                                                                                        \
	}

#define IS_DATA_INIT( ret )                                                                                                                \
	if ( !m_pTerrain || !m_pTerrain->getData() ) {                                                                                         \
		return ret;                                                                                                                        \
	}

#define IS_DATA_INIT_MESG( mesg, ret )                                                                                                     \
	if ( !m_pTerrain || !m_pTerrain->getData() ) {                                                                                         \
		LOG( ERROR, mesg );                                                                                                                \
		return ret;                                                                                                                        \
	}


// Additional migrated constants.
#define s_pszNativeBuildId ( "instance-local-active-storage-p2-2026-09-09" )
#define s_pszEngineSingletonName ( "Terrain3DForwardPlusBridge" )
#define s_pszProfileKey ( "xbox360_experimental" )
#define s_pszProfileDisplayName ( "Xbox 360 Experimental" )
static const char *const g_t3dConstStorage_s_pszCacheThrashSubsystemNames[] = {
	"gpu_terrain_pages",
	"cpu_terrain_pages",
	"instance_pages",
};
#define s_pszCacheThrashSubsystemNames g_t3dConstStorage_s_pszCacheThrashSubsystemNames
static const char *const g_t3dConstStorage_s_pszMemorySubsystemNames[] = {
	"gpu_terrain_pages",
	"cpu_terrain_pages",
	"instance_pages",
	"baked_lighting",
	"hlod_proxies",
	"shader_cache",
};
#define s_pszMemorySubsystemNames g_t3dConstStorage_s_pszMemorySubsystemNames
static const char *const g_t3dConstStorage_s_pszEditorToolNames[] = {
	"Region",
	"Sculpt",
	"Height",
	"Texture",
	"Color",
	"Roughness",
	"Auto Shader",
	"Holes",
	"Navigation",
	"Instancer",
	"Angle",
	"Scale",
	"Landform",
	"TOOL_MAX",
};
#define s_pszEditorToolNames g_t3dConstStorage_s_pszEditorToolNames
static const char *const g_t3dConstStorage_s_pszEditorOperationNames[] = {
	"Add",
	"Subtract",
	"Replace",
	"Average",
	"Gradient",
	"OP_MAX",
};
#define s_pszEditorOperationNames g_t3dConstStorage_s_pszEditorOperationNames
static const char *const g_t3dConstStorage_s_pszShaderStaticMacroNames[] = {
	"T3D_TERRAIN_SAMPLING_MODE",
	"T3D_TERRAIN_NORMAL_MODE",
	"T3D_COLOR_SAMPLING_MODE",
	"T3D_PROJECTION_MODE",
	"T3D_TEXTURE_SURFACE_MODE",
	"T3D_SAMPLE_SURFACE_DATA",
	"T3D_OUTPUT_ROUGHNESS",
	"T3D_OUTPUT_SPECULAR",
	"T3D_OUTPUT_NORMAL_MAP",
	"T3D_OUTPUT_AO",
	"T3D_SHADING_MODE",
	"T3D_DETILING_MODE",
	"T3D_WORLD_NOISE_SHADER_MODE",
	"T3D_FAST_BLEND_CURVE",
	"T3D_CONTROL_MAPS_IMPLICIT",
	"T3D_COLOR_MAPS_IMPLICIT",
	"T3D_AUTO_SHADER_ENABLED",
};
#define s_pszShaderStaticMacroNames g_t3dConstStorage_s_pszShaderStaticMacroNames
static const char *const g_t3dConstStorage_s_pszRegionMapTypeNames[s_nTypeMaxValue + 1u] = {
	"TYPE_HEIGHT",
	"TYPE_CONTROL",
	"TYPE_COLOR",
	"TYPE_MAX",
};
#define s_pszRegionMapTypeNames g_t3dConstStorage_s_pszRegionMapTypeNames
static const char *const g_t3dConstStorage_s_pszShaderFeatures[] = {
	"AUTO_SHADER",
	"AUTO_SHADER_UNIFORMS",
	"BLEND_WEIGHT_FUNCTIONS",
	"CLIPMAP_BATCHING_FUNCTIONS",
	"DISPLACEMENT_FUNCTIONS",
	"DISPLACEMENT_UNIFORMS",
	"DISPLACEMENT_VERTEX",
	"DUAL_SCALING",
	"DUAL_SCALING_CONDITION_0",
	"DUAL_SCALING_CONDITION_1",
	"DUAL_SCALING_FACTOR",
	"DUAL_SCALING_MIX",
	"DUAL_SCALING_UNIFORMS",
	"FAR_FLAT_FUNCTIONS",
	"FAR_NONE_FRAGMENT",
	"FAR_NONE_FUNCTIONS",
	"FLAT_FRAGMENT",
	"FLAT_FUNCTIONS",
	"FLAT_UNIFORMS",
	"FLAT_VERTEX",
	"LEGACY_PBR_DISPLACEMENT_FUNCTIONS",
	"LEGACY_PBR_MATERIAL_FUNCTIONS",
	"LEGACY_PBR_UNIFORMS",
	"MACRO_VARIATION",
	"MACRO_VARIATION_UNIFORMS",
	"MIDDLE_NONE_FRAGMENT",
	"NONE_CHECK",
	"NONE_FUNCTIONS",
	"OUTPUT_ALBEDO",
	"OUTPUT_ALBEDO_GREY",
	"OUTPUT_AMBIENT_OCCLUSION",
	"OUTPUT_METALLIC",
	"OUTPUT_NORMAL_MAP",
	"OUTPUT_ROUGHNESS",
	"OUTPUT_SPECULAR",
	"OUTPUT_SPECULAR_NONE",
	"PROJECTION",
	"SURFACE_DISPLACEMENT_UNIFORM",
	"SURFACE_PBR_CORE_FUNCTIONS",
	"SURFACE_PBR_DISPLACEMENT_FUNCTIONS",
	"SURFACE_PBR_MATERIAL_FUNCTIONS",
	"SURFACE_PBR_UNIFORMS",
	"TERRAIN_MATERIAL_SAMPLING",
	"TEXTURE_SAMPLERS_LINEAR",
	"TEXTURE_SAMPLERS_LINEAR_ANISOTROPIC",
	"TEXTURE_SAMPLERS_NEAREST",
	"TEXTURE_SAMPLERS_NEAREST_ANISOTROPIC",
	"WORLD_NOISE_FRAGMENT",
	"WORLD_NOISE_FUNCTIONS",
	"WORLD_NOISE_UNIFORMS",
	"WORLD_NOISE_VERTEX",
};
#define s_pszShaderFeatures g_t3dConstStorage_s_pszShaderFeatures
static const uint32_t g_t3dConstStorage_s_szSmallClasses[s_nSmallClassCount] = { 16u, 32u, 64u, 128u, 256u, 512u, 1024u, 2048u, 4096u };
#define s_szSmallClasses g_t3dConstStorage_s_szSmallClasses

#endif // TERRAIN3D_CONSTANTS_H
