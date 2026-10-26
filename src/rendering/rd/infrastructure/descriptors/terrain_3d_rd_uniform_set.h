#ifndef TERRAIN3D_RD_UNIFORM_SET_H
#define TERRAIN3D_RD_UNIFORM_SET_H

#include "rendering/rd/core/terrain_3d_rd_context.h"

#include <godot_cpp/classes/rd_uniform.hpp>
#include <godot_cpp/variant/typed_array.hpp>

// Owns one physical descriptor set. Replacement is transactional: a failed
// creation never changes the RID already held by this object.
class Terrain3DRdUniformSet {
	RID m_rid;
	t3dRdResourceOwner_t m_owner = T3D_RD_CONTEXT_OWNER_UNSPECIFIED;
	uint32_t m_generation = 0u;
	bool m_local = false;

	static uint32_t nextGeneration( uint32_t value ) { return value == UINT32_MAX ? 1u : value + 1u; }

public:
	Terrain3DRdUniformSet() = default;
	Terrain3DRdUniformSet( const Terrain3DRdUniformSet & ) = delete;
	Terrain3DRdUniformSet &operator=( const Terrain3DRdUniformSet & ) = delete;

	const RID &rid() const { return m_rid; }
	bool isValid() const { return m_rid.is_valid(); }
	uint32_t generation() const { return m_generation; }
	t3dRdResourceOwner_t owner() const { return m_owner; }
	bool isLocal() const { return m_local; }

	bool createTracked( Terrain3DRdContext &context, t3dRdResourceOwner_t owner,
	        const String &name, const TypedArray<Ref<RDUniform>> &uniforms, const RID &shader, uint32_t set );
	bool createLocal( RenderingDevice *rd, const TypedArray<Ref<RDUniform>> &uniforms,
	        const RID &shader, uint32_t set );
	void releaseTracked( Terrain3DRdContext &context );
	void releaseLocal( RenderingDevice *rd );
	void abandon();
	void swap( Terrain3DRdUniformSet &other );
};

#endif // TERRAIN3D_RD_UNIFORM_SET_H
