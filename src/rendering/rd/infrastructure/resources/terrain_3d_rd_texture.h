#ifndef TERRAIN3D_RD_TEXTURE_H
#define TERRAIN3D_RD_TEXTURE_H

#include "rendering/rd/infrastructure/resources/terrain_3d_rd_resource_state.h"

#include <stdint.h>
#include <godot_cpp/classes/rd_texture_format.hpp>
#include <godot_cpp/classes/rd_texture_view.hpp>
#include <godot_cpp/variant/callable.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/typed_array.hpp>
#include <godot_cpp/variant/vector3.hpp>

using namespace godot;

typedef struct t3dRdTextureLayout_s {

	RenderingDevice::DataFormat m_format = RenderingDevice::DATA_FORMAT_MAX;
	RenderingDevice::TextureType m_type = RenderingDevice::TEXTURE_TYPE_2D;
	RenderingDevice::TextureSamples m_samples = RenderingDevice::TEXTURE_SAMPLES_1;
	uint32_t m_nWidth = 0u;
	uint32_t m_nHeight = 0u;
	uint32_t m_nDepth = 1u;
	uint32_t m_nLayers = 0u;
	uint32_t m_nMipmaps = 0u;
	uint64_t m_nUsageBits = 0u;
	uint64_t m_nBytes = 0u;

	EA_FORCE_INLINE bool valid( void ) const {
		return m_format != RenderingDevice::DATA_FORMAT_MAX && m_nWidth > 0u && m_nHeight > 0u && m_nDepth > 0u
		        && m_nLayers > 0u && m_nMipmaps > 0u && m_nBytes > 0u;
	}
} t3dRdTextureLayout_t;


typedef struct t3dRdTextureSpec_s {

	String m_name;
	t3dRdResourceOwner_t m_owner = T3D_RD_CONTEXT_OWNER_UNSPECIFIED;
	t3dRdMemoryCategory_t m_memory = T3D_RD_CONTEXT_MEMORY_TERRAIN_TEXTURE;
	t3dRdTextureLayout_t m_layout;
	bool m_bFailureFatal = true;
} t3dRdTextureSpec_t;


typedef struct t3dRdTextureSliceSpec_s {

	String m_name;
	t3dRdResourceOwner_t m_owner = T3D_RD_CONTEXT_OWNER_UNSPECIFIED;
	t3dRdMemoryCategory_t m_memory = T3D_RD_CONTEXT_MEMORY_NONE;
	uint32_t m_nLayer = 0u;
	uint32_t m_nMipmap = 0u;
	uint32_t m_nMipmaps = 1u;
	RenderingDevice::TextureSliceType m_sliceType = RenderingDevice::TEXTURE_SLICE_2D;
} t3dRdTextureSliceSpec_t;


class Terrain3DRdTexture {
	Terrain3DMemoryReservation m_memoryReservation;
	Terrain3DRdResourceState<1u> m_state;
	t3dRdTextureLayout_t m_layout;
	bool m_bSharedView = false;

	static uint32_t maximumMipCount( uint32_t nWidth, uint32_t nHeight );
	static bool validateLayout( const t3dRdTextureLayout_t &rLayout );
	static Ref<RDTextureFormat> buildFormat( const t3dRdTextureLayout_t &rLayout );
	static bool registerTracked( Terrain3DRdContext &rContext, RenderingDevice *pRd, RID &rRid,
	        const String &rName, t3dRdResourceOwner_t owner,
	        t3dRdMemoryCategory_t memory, uint64_t nBytes, bool bFailureFatal );

public:
	Terrain3DRdTexture( void ) = default;
	Terrain3DRdTexture( const Terrain3DRdTexture & ) = delete;
	Terrain3DRdTexture &operator=( const Terrain3DRdTexture & ) = delete;

	EA_FORCE_INLINE bool isValid( void ) const { return m_state.isValid(); }
	EA_FORCE_INLINE const RID &rid( void ) const { return m_state.rid(); }
	EA_FORCE_INLINE const t3dRdTextureLayout_t &layout( void ) const { return m_layout; }
	EA_FORCE_INLINE uint64_t bytes( void ) const { return m_layout.m_nBytes; }
	EA_FORCE_INLINE uint32_t generation( void ) const { return m_state.generation(); }
	EA_FORCE_INLINE bool isSharedView( void ) const { return m_bSharedView; }
	static bool isBorrowedValid( RenderingDevice *pRd, const RID &rRid );
	static Ref<RDTextureFormat> borrowedFormat( RenderingDevice *pRd, const RID &rRid );
	static bool supportsFormatUsage( RenderingDevice *pRd, RenderingDevice::DataFormat format,
	        BitField<RenderingDevice::TextureUsageBits> usage );

	bool createTracked( Terrain3DRdContext &rContext, const t3dRdTextureSpec_t &rSpec,
	        const TypedArray<PackedByteArray> &rInitialData = TypedArray<PackedByteArray>() );
	bool createSharedTracked( Terrain3DRdContext &rContext, const Terrain3DRdTexture &rParent,
	        const t3dRdTextureSliceSpec_t &rSpec, const Ref<RDTextureView> &rView = Ref<RDTextureView>() );
	bool updateLayerTracked( Terrain3DRdContext &rContext, uint32_t nLayer, const PackedByteArray &rData,
	        Terrain3DGpuBufferPolicy policy = T3D_GPU_DIRTY_UPLOAD ) const;
	bool copyRegionToTracked( Terrain3DRdContext &rContext, Terrain3DRdTexture &rDestination,
	        const Vector3 &rFrom, const Vector3 &rTo, const Vector3 &rSize,
	        uint32_t nSrcMipmap = 0u, uint32_t nDstMipmap = 0u, uint32_t nSrcLayer = 0u, uint32_t nDstLayer = 0u ) const;
	PackedByteArray readbackTracked( Terrain3DRdContext &rContext, uint32_t nLayer ) const;
	Error readbackAsyncTracked( Terrain3DRdContext &rContext, uint32_t nLayer, const Callable &rCallback ) const;
	void releaseTracked( Terrain3DRdContext &rContext );
	void abandon( void );
	void swap( Terrain3DRdTexture &rOther );
};

#endif // TERRAIN3D_RD_TEXTURE_H
