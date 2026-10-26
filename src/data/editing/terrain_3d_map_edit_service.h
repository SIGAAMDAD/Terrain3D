// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_MAP_EDIT_SERVICE_H
#define TERRAIN3D_MAP_EDIT_SERVICE_H

#include "Terrain3DTypes.h"

#include "core/runtime/terrain_3d_eastl_helpers.h"
#include "Terrain3DConstants.h"
#include "data/model/terrain_3d_region.h"
#include "eastl_config.h"

#include <EASTL/vector.h>
#include <stddef.h>
#include <stdint.h>
#include <godot_cpp/variant/aabb.hpp>
#include <godot_cpp/variant/rect2i.hpp>
#include <godot_cpp/variant/typed_array.hpp>
#include "data/persistence/terrain_3d_region_repository.h"
#include "data/sampling/terrain_3d_sampling_storage.h"
#include "rendering/compute/terrain_3d_compute.h"
#include "rendering/material/terrain_3d_material_tile_class.h"

// Owns one synchronous edit dispatch. Vectors are moved out of the service's
// retained scratch so synchronous Godot signals can safely trigger nested edits.
// recycleMutation() returns their capacity to the service after dispatch.
typedef struct t3dMapMutation_s
{
	t3dMapType_t													m_mapType = REGION_TYPE_MAX;
	AABB															m_editedArea;
	Vector2															m_editedHeightRange = s_v2Zero;
	eastl::vector< Vector2i, Terrain3DEastlAllocator >				m_changedLocations;
	eastl::vector< Vector2i, Terrain3DEastlAllocator >				m_derivedLocations;
	eastl::vector< t3dDerivedMapUpdate_t, Terrain3DEastlAllocator > m_derivedUpdates;

	EA_FORCE_INLINE bool valid(
		void ) const {
		return m_mapType >= TYPE_HEIGHT && m_mapType < REGION_TYPE_MAX && !m_changedLocations.empty();
	}
} t3dMapMutation_t;

typedef bool ( *t3dMapEditEnsureRegionResidentFn_t )( void *pContext, const Vector2i &rLocation );
typedef Error ( *t3dMapEditAcquireEditableRegionFn_t )( void				   *pContext,
														const Vector2i		   &rLocation,
														Ref< Terrain3DRegion > &rRegion );

// Owns authoritative terrain-map mutation mechanics and edit bookkeeping.
// Renderer publication, far-atlas invalidation, collision/instance updates,
// terrain snapping, and Godot signal emission remain integration concerns.
class Terrain3DMapEditService
{
public:
	Terrain3DMapEditService( Terrain3DRegionRepository *pRepository, Terrain3DSamplingStorage *pSamplingStorage );
	Terrain3DMapEditService( const Terrain3DMapEditService & ) = delete;
	Terrain3DMapEditService &operator=( const Terrain3DMapEditService & ) = delete;

	void reset( void );
	void releaseRuntimeAllocatorStorage( void );

	Error setPixel( t3dMapType_t   eMapType,
					const Vector3 &rGlobalPosition,
					const Color	  &rPixel,
					Vector2i	  &rRegionLocation,
					Rect2i		  &rDirtyRect );

	t3dMapMutation_t commitMapEdits( t3dMapType_t											   eMapType,
									 const eastl::vector< Vector2i, Terrain3DEastlAllocator > &rRegionLocations,
									 const AABB												  &rEditedArea,
									 Terrain3DCompute										  *pCompute );
	void			 finalizeMapEdits( const t3dMapMutation_t &rMutation, bool bKeepEdited );
	void			 recycleMutation( t3dMapMutation_t &&rMutation );

	const eastl::vector< t3dDerivedMapUpdate_t, Terrain3DEastlAllocator > &
	regenerateDerivedMaps( const eastl::vector< Vector2i, Terrain3DEastlAllocator > &rRegionLocations,
						   const AABB												&rEditedArea,
						   Terrain3DCompute											*pCompute );

	bool buildRegionEditArea( const Vector2i &rRegionLocation, const Rect2i &rDirtyRect, AABB &rEditedArea ) const;

	void				 addEditedArea( const AABB &rArea );
	EA_FORCE_INLINE void clearEditedArea(
		void ) {
		m_editedArea = AABB();
	}
	EA_FORCE_INLINE const AABB &getEditedArea(
		void ) const {
		return m_editedArea;
	}

	EA_FORCE_INLINE void resetHeightRange(
		void ) {
		m_masterHeightRange = s_v2Zero;
	}
	EA_FORCE_INLINE const Vector2 &getHeightRange(
		void ) const {
		return m_masterHeightRange;
	}
	void updateMasterHeight( real_t fHeight );
	void updateMasterHeights( const Vector2 &rLowHigh );
	void calculateHeightRange( bool								  bRecursive = false,
							   t3dMapEditEnsureRegionResidentFn_t pEnsureResident = NULL,
							   void								 *pResidentContext = NULL );

	void copyRegionMaps( const Terrain3DRegion *pSourceRegion,
						 const Rect2i		   &rSourceRect,
						 const Rect2i		   &rDestinationRect,
						 Terrain3DRegion	   *pDestinationRegion ) const;

	Error importImages( const TypedArray< Image >		   &rImages,
						const Vector3					   &rGlobalPosition,
						real_t								fOffset,
						real_t								fScale,
						t3dMapEditAcquireEditableRegionFn_t pAcquireRegion,
						void							   *pAcquireContext,
						bool							   &rGenerateMipmaps );

	Ref< Image > layeredToImage( t3dMapType_t						eMapType,
								 t3dMapEditEnsureRegionResidentFn_t pEnsureResident = NULL,
								 void							   *pResidentContext = NULL ) const;

    const t3dMaterialTileRegionClasses_t *getControlTileClassesCold( const Vector2i &rRegionLocation ) const;
    void invalidateControlTileClassesCold( const Vector2i &rLocation );
    void refreshControlTileClassesCold( const Vector2i &rLocation, const Terrain3DRegion *pRegion );

	size_t getTransientScratchBytes( void ) const;

	static Rect2i getRegionDirtyRect( const Vector2i &rLocation, const AABB &rEditedArea, int nMargin = 0 );

private:
    // Region-aligned, fixed-capacity derived metadata. Never used as an authority
    // for rendering until a complete draw batch has been certified.
    t3dMaterialTileRegionClasses_t m_controlTileClasses[T3D_REGION_SLOTS]{};
    void rebuildControlTileClassesCold( const Vector2i &rLocation, const Terrain3DRegion *pRegion );

	Terrain3DRegionRepository *m_pRepository = NULL;
	Terrain3DSamplingStorage  *m_pSamplingStorage = NULL;

	AABB	m_editedArea;
	Vector2 m_masterHeightRange = s_v2Zero;

	// Reused on the synchronous edit path. Worker tasks receive retained, disjoint halo spans.
	eastl::vector< t3dDerivedMapTask_t, Terrain3DEastlAllocator >	m_derivedTaskScratch;
	eastl::vector< float, Terrain3DEastlAllocator >					m_fDerivedWorkerHaloScratch;
	eastl::vector< uint8_t, Terrain3DEastlAllocator >				m_derivedOutputScratch;
	eastl::vector< Vector2i, Terrain3DEastlAllocator >				m_activeLocationScratch;
	eastl::vector< Vector2i, Terrain3DEastlAllocator >				m_derivedLocationScratch;
	eastl::vector< t3dDerivedMapUpdate_t, Terrain3DEastlAllocator > m_derivedUpdateScratch;

	t3dMapMutation_t
	makeMutationResult( t3dMapType_t eMapType, const AABB &rEditedArea, const Vector2 &rEditedHeightRange );
};

#endif // TERRAIN3D_MAP_EDIT_SERVICE_H
