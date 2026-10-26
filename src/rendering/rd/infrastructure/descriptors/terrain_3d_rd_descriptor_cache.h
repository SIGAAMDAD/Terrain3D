#ifndef TERRAIN3D_RD_DESCRIPTOR_CACHE_H
#define TERRAIN3D_RD_DESCRIPTOR_CACHE_H

#include "rendering/rd/infrastructure/descriptors/terrain_3d_rd_uniform_set.h"
#include "rendering/rd/infrastructure/resources/terrain_3d_rd_resource_cache.h"

// Key equality is supplied by the domain. Only shader, layout and bound RID
// identities belong in keys; per-frame buffer contents do not.
template <typename TKey, uint32_t Capacity>
class Terrain3DRdDescriptorCache : public Terrain3DRdResourceCache<TKey, Terrain3DRdUniformSet, Capacity, EVENT_DESCRIPTOR_EVICT> {
	using Cache = Terrain3DRdResourceCache<TKey, Terrain3DRdUniformSet, Capacity, EVENT_DESCRIPTOR_EVICT>;
	using Cache::insert;

public:
	Terrain3DRdDescriptorCache( Terrain3DRdContext &rContext, t3dRdResourceOwner_t owner ) :
	        Cache( rContext, owner ) {}

	RID find( const TKey &rKey ) const {
		const auto *const entry = Cache::find( rKey );
		return entry ? entry->resource().rid() : RID();
	}

	RID getOrCreate( const TKey &rKey,
	        const String &rName,
	        const TypedArray<Ref<RDUniform>> &rUniforms,
	        const RID &rShader,
	        uint32_t nSetIndex ) {
		const RID existing = find( rKey );
		if ( existing.is_valid() ) {
			return existing;
		}
		Terrain3DRdUniformSet created;
		if ( !created.createTracked( this->m_context, this->m_owner, rName, rUniforms, rShader, nSetIndex ) ) {
			return RID();
		}
		return this->insert( rKey, created );
	}
};

#endif // TERRAIN3D_RD_DESCRIPTOR_CACHE_H
