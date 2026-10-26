// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_RD_FORWARD_LIGHTING_H
#define TERRAIN3D_RD_FORWARD_LIGHTING_H

#include "Terrain3DTypes.h"

#include "Terrain3DConstants.h"
#include "rendering/rd/core/terrain_3d_rd_context.h"
#include "rendering/rd/infrastructure/descriptors/terrain_3d_rd_descriptor_cache.h"
#include "rendering/rd/infrastructure/buffers/terrain_3d_rd_gpu_buffer.h"
#include "rendering/rd/infrastructure/resources/terrain_3d_rd_sampler.h"
#include "rendering/rd/infrastructure/resources/terrain_3d_rd_texture.h"
#include "rendering/rd/core/terrain_3d_rd_types.h"

#include <stddef.h>
#include <string.h>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>

using namespace godot;

// SH-B frozen shared Forward+ descriptor ABI. The shader keeps literal
// set/binding numbers so SH-A descriptor reflection remains capable of catching
// drift; C++ construction uses these names instead of parallel magic numbers.
#define T3D_RD_FORWARD_LIGHTING_SHARED_CONTRACT_VERSION 1u
#define T3D_RD_FORWARD_LIGHTING_DESCRIPTOR_SET 2u
#define T3D_RD_FORWARD_LIGHTING_FRAME_BINDING 0u
#define T3D_RD_FORWARD_LIGHTING_DIRECTIONAL_BINDING 1u
#define T3D_RD_FORWARD_LIGHTING_OMNI_BINDING 2u
#define T3D_RD_FORWARD_LIGHTING_SPOT_BINDING 3u
#define T3D_RD_FORWARD_LIGHTING_AREA_BINDING 4u
#define T3D_RD_FORWARD_LIGHTING_CLUSTER_BINDING 5u
#define T3D_RD_FORWARD_LIGHTING_SHADOW_SAMPLER_BINDING 6u
#define T3D_RD_FORWARD_LIGHTING_SHADOW_ATLAS_BINDING 7u
#define T3D_RD_FORWARD_LIGHTING_DIRECTIONAL_SHADOW_BINDING 8u
#define T3D_RD_FORWARD_LIGHTING_REFLECTION_BUFFER_BINDING 9u
#define T3D_RD_FORWARD_LIGHTING_REFLECTION_ATLAS_BINDING 10u
#define T3D_RD_FORWARD_LIGHTING_DECAL_BUFFER_BINDING 11u
#define T3D_RD_FORWARD_LIGHTING_DECAL_ATLAS_BINDING 12u
#define T3D_RD_FORWARD_LIGHTING_DECAL_ATLAS_SRGB_BINDING 13u
#define T3D_RD_FORWARD_LIGHTING_SCENE_SAMPLER_BINDING 14u
#define T3D_RD_FORWARD_LIGHTING_RADIANCE_BINDING 15u
#define T3D_RD_FORWARD_LIGHTING_BINDING_COUNT 16u

// Terrain3D-owned descriptor adapter for borrowed Godot Forward+ lighting state.
// Borrowed engine RIDs are never registered or freed here. Only the small frame
// UBO, fallback shadow resources, and shader-specific descriptor sets are owned.
class Terrain3DRdForwardLighting {
private:
static_assert( sizeof( t3dRdForwardLightFrame_t ) == 160u, "Forward lighting UBO ABI changed" );
	static_assert( offsetof( t3dRdForwardLightFrame_t, m_szAmbientLightColorEnergy ) == 64u, "Forward ambient UBO offset changed" );
	static_assert( offsetof( t3dRdForwardLightFrame_t, m_szEnvironmentParams0 ) == 80u, "Forward Environment UBO offset changed" );
	static_assert( offsetof( t3dRdForwardLightFrame_t, m_szEnvironmentParams1 ) == 96u, "Forward environment tail UBO offset changed" );
	static_assert( offsetof( t3dRdForwardLightFrame_t, m_szRadianceInverseXform ) == 112u, "Forward radiance matrix UBO offset changed" );
	static_assert( T3D_RD_FORWARD_LIGHTING_DESCRIPTOR_SET == 2u, "Forward lighting descriptor set changed" );
	static_assert( T3D_RD_FORWARD_LIGHTING_RADIANCE_BINDING == 15u, "Forward lighting descriptor layout changed" );
	static_assert( T3D_RD_FORWARD_LIGHTING_BINDING_COUNT == 16u, "Forward lighting descriptor count changed" );
	EA_FORCE_INLINE static void initFrame( t3dRdForwardLightFrame_t *pFrame ) {
		memset( pFrame, 0, sizeof( *pFrame ) );
		pFrame->m_nDefaultSurfaceMask = UINT32_MAX;
	}

	typedef struct t3dRdForwardLightKey_s {

		RID m_hDirectional;
		RID m_hOmni;
		RID m_hSpot;
		RID m_hArea;
		RID m_hCluster;
		RID m_hShadowAtlas;
		RID m_hDirectionalShadowAtlas;
		RID m_hShadowSampler;
		RID m_hReflectionProbeBuffer;
		RID m_hReflectionAtlas;
		RID m_hDecalBuffer;
		RID m_hDecalAtlas;
		RID m_hDecalAtlasSrgb;
		RID m_hRadiance;
		bool m_bOneSun = false;
		uint32_t m_nBorrowedGeneration = 0u;

		bool operator==( const t3dRdForwardLightKey_s &rOther ) const {
			return m_hDirectional == rOther.m_hDirectional && m_hOmni == rOther.m_hOmni && m_hSpot == rOther.m_hSpot
			        && m_hArea == rOther.m_hArea && m_hCluster == rOther.m_hCluster && m_hShadowAtlas == rOther.m_hShadowAtlas
			        && m_hDirectionalShadowAtlas == rOther.m_hDirectionalShadowAtlas && m_hShadowSampler == rOther.m_hShadowSampler
			        && m_hReflectionProbeBuffer == rOther.m_hReflectionProbeBuffer && m_hReflectionAtlas == rOther.m_hReflectionAtlas
			        && m_hDecalBuffer == rOther.m_hDecalBuffer && m_hDecalAtlas == rOther.m_hDecalAtlas
		        && m_hDecalAtlasSrgb == rOther.m_hDecalAtlasSrgb && m_hRadiance == rOther.m_hRadiance && m_bOneSun == rOther.m_bOneSun
		        && m_nBorrowedGeneration == rOther.m_nBorrowedGeneration;
		}
	} t3dRdForwardLightKey_t;


	typedef struct t3dRdForwardDescriptorKey_s {

		RID shader;
		RID frame;
		RID sampler;
		t3dRdForwardLightKey_t resources;
		uint64_t deviceGeneration = 0u;
		bool operator==( const t3dRdForwardDescriptorKey_s &rOther ) const {
			return shader == rOther.shader && frame == rOther.frame && sampler == rOther.sampler && resources == rOther.resources
			        && deviceGeneration == rOther.deviceGeneration;
		}
	} t3dRdForwardDescriptorKey_t;

	typedef Terrain3DRdDescriptorCache<t3dRdForwardDescriptorKey_t, T3D_MAX_SETS> t3dRdForwardDescriptorCache_t;

	Terrain3DRdContext &m_context;
	Terrain3DRdGpuBuffer m_hFrameBuffer;
	Terrain3DRdTexture m_hFallbackShadowTexture;
	Terrain3DRdSampler m_hFallbackShadowSampler;
	Terrain3DRdGpuBuffer m_hFallbackStorageBuffer;
	Terrain3DRdTexture m_hFallbackColorTexture;
	Terrain3DRdTexture m_hFallbackColorArrayTexture;
	Terrain3DRdTexture m_hFallbackRadianceTexture;
	Terrain3DRdTexture m_hFallbackRadianceArrayTexture;
	Terrain3DRdSampler m_hLinearSampler;
	t3dRdForwardLightKey_t m_key{};
	bool m_bKeyValid = false;
	uint64_t m_nDeviceGeneration = 0u;
	uint64_t m_nFrameGeneration = 0u;
	uint32_t m_nFrameSurfaceMask = 0u;
	uint64_t m_nSceneResourceChanges = 0u;
	uint64_t m_nEnvironmentFrameUpdates = 0u;
	uint64_t m_nEnvironmentResourceChanges = 0u;
	uint64_t m_nEnvironmentInvalidResourceCount = 0u;
	uint64_t m_nLastInvalidEnvironmentFrameGeneration = 0u;
	uint32_t m_nLastEnvironmentFlags = 0u;
	float m_szLastAmbientLightColorEnergy[4]{};
	float m_fLastAmbientColorSkyMix = 0.0f;
	float m_fLastIblExposureNormalization = 1.0f;
	float m_fLastEmissiveExposureNormalization = 1.0f;
	bool m_bLastRadianceValid = false;
	bool m_bPersistentFrameBuffer = false;

	t3dRdForwardDescriptorCache_t m_descriptors;
	PackedByteArray m_frameBytes;
	String m_lastError;

	void setError( const String &rError );
	bool ensureOwnedResources( void );
	void freeSets( void );
	t3dRdForwardDescriptorKey_t descriptorKey( const RID &rShader ) const;
	RID resolveShadowTexture( const RID &rNative ) const;
	RID resolveShadowSampler( const RID &rNative ) const;
	RID resolveStorageBuffer( const RID &rNative ) const;
	RID resolveColorTexture( const RID &rNative ) const;
	RID resolveColorArrayTexture( const RID &rNative ) const;
	RID resolveRadianceTexture( const RID &rNative, bool bArray ) const;

public:
	explicit Terrain3DRdForwardLighting( Terrain3DRdContext &rContext );
	~Terrain3DRdForwardLighting( void );

	EA_FORCE_INLINE void setPersistentFrameBufferEnabled( const bool bEnabled ) {
		m_bPersistentFrameBuffer = bEnabled;
	}
	bool prepare( const t3dRdForwardPassContext_t &rContext, uint32_t nDefaultSurfaceMask, bool bOneSun = false );
	RID ensureSet( const RID &rShader );
	RID getSet( const RID &rShader ) const;
	void invalidateBorrowedState( void );
	void shutdown( void );
	Dictionary getDiagnostics( void ) const;
};

#endif // TERRAIN3D_RD_FORWARD_LIGHTING_H
