#ifndef TERRAIN3D_RD_GEOMETRY_VIEW_H
#define TERRAIN3D_RD_GEOMETRY_VIEW_H

#include "rendering/rd/infrastructure/resources/terrain_3d_rd_resource_state.h"

#include <stdint.h>
#include <godot_cpp/variant/packed_int64_array.hpp>
#include <godot_cpp/variant/typed_array.hpp>

using namespace godot;

// Cohesive owner for RenderingDevice vertex/index array views. The underlying
// vertex/index buffers remain externally owned by Terrain3DRdGpuBuffer.
class Terrain3DRdGeometryView {
	Terrain3DRdResourceState<2u> m_state;

	bool ownerCompatible( t3dRdResourceOwner_t owner ) const;

public:
	Terrain3DRdGeometryView( void ) = default;
	Terrain3DRdGeometryView( const Terrain3DRdGeometryView & ) = delete;
	Terrain3DRdGeometryView &operator=( const Terrain3DRdGeometryView & ) = delete;
	Terrain3DRdGeometryView( Terrain3DRdGeometryView &&rOther ) noexcept;
	Terrain3DRdGeometryView &operator=( Terrain3DRdGeometryView && ) = delete;

	EA_FORCE_INLINE bool hasVertex( void ) const { return m_state.rid().is_valid(); }
	EA_FORCE_INLINE bool hasIndex( void ) const { return m_state.rid( 1u ).is_valid(); }
	EA_FORCE_INLINE bool isValid( void ) const { return m_state.isValid(); }
	EA_FORCE_INLINE const RID &vertexRid( void ) const { return m_state.rid(); }
	EA_FORCE_INLINE const RID &indexRid( void ) const { return m_state.rid( 1u ); }
	EA_FORCE_INLINE uint32_t generation( void ) const { return m_state.generation(); }
	EA_FORCE_INLINE t3dRdResourceOwner_t owner( void ) const { return m_state.owner(); }

	bool createVertexTracked( Terrain3DRdContext &rContext, t3dRdResourceOwner_t owner,
	        const String &rName, uint32_t nVertexCount, int64_t nVertexFormat,
	        const TypedArray<RID> &rBuffers, const PackedInt64Array &rOffsets = PackedInt64Array() );
	bool createIndexTracked( Terrain3DRdContext &rContext, t3dRdResourceOwner_t owner,
	        const String &rName, const RID &rBuffer, uint32_t nOffset, uint32_t nCount );
	bool createIndexedTracked( Terrain3DRdContext &rContext, t3dRdResourceOwner_t owner,
	        const String &rVertexName, const String &rIndexName,
	        uint32_t nVertexCount, int64_t nVertexFormat,
	        const TypedArray<RID> &rVertexBuffers, const PackedInt64Array &rVertexOffsets,
	        const RID &rIndexBuffer, uint32_t nIndexOffset, uint32_t nIndexCount );
	void releaseTracked( Terrain3DRdContext &rContext );
	void abandon( void );
	void swap( Terrain3DRdGeometryView &rOther );
};

#endif // TERRAIN3D_RD_GEOMETRY_VIEW_H
