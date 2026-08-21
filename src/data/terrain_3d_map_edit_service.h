// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#pragma once

#ifndef TERRAIN3D_MAP_EDIT_SERVICE_H
#define TERRAIN3D_MAP_EDIT_SERVICE_H

#include "Terrain3DConstants.h"
#include "data/terrain_3d_region.h"

#include <godot_cpp/variant/aabb.hpp>
#include <godot_cpp/variant/rect2i.hpp>
#include <godot_cpp/variant/typed_array.hpp>

#include "eastl_config.h"
#include <EASTL/vector.h>
#include <cstddef>
#include <cstdint>

class T3DCompute;
class T3DRegionRepository;
class T3DSamplingStorage;

struct T3DDerivedMapUpdate {
	Vector2i location = V2I_MAX;
	Rect2i dirtyRect;
	bool bSuccess = false;
};

// Owns one synchronous edit dispatch. Vectors are moved out of the service's
// retained scratch so synchronous Godot signals can safely trigger nested edits.
// recycleMutation() returns their capacity to the service after dispatch.
struct T3DMapMutation {
	t3dmapType_t mapType = T3DRegion::TYPE_MAX;
	AABB editedArea;
	Vector2 editedHeightRange = V2_ZERO;
	eastl::vector<Vector2i> changedLocations;
	eastl::vector<Vector2i> derivedLocations;
	eastl::vector<T3DDerivedMapUpdate> derivedUpdates;

	EA_FORCE_INLINE bool valid( void ) const { return mapType >= T3DRegion::TYPE_HEIGHT && mapType < T3DRegion::TYPE_MAX && !changedLocations.empty(); }
};

// Owns authoritative terrain-map mutation mechanics and edit bookkeeping.
// Renderer publication, far-atlas invalidation, collision/instance updates,
// terrain snapping, and Godot signal emission remain integration concerns.
class T3DMapEditService {
public:
	using EnsureRegionResidentFn = bool ( * )( void *pContext, const Vector2i &rLocation );
	using AcquireEditableRegionFn = Error ( * )( void *pContext, const Vector2i &rLocation, Ref<T3DRegion> &rRegion );

	T3DMapEditService( T3DRegionRepository *pRepository, T3DSamplingStorage *pSamplingStorage );
	T3DMapEditService( const T3DMapEditService & ) = delete;
	T3DMapEditService &operator=( const T3DMapEditService & ) = delete;

	void reset( void );

	Error setPixel(
		t3dmapType_t eMapType, const Vector3 &rGlobalPosition, const Color &rPixel,
		Vector2i &rRegionLocation, Rect2i &rDirtyRect
	);

	T3DMapMutation commitMapEdits(
		t3dmapType_t eMapType, const eastl::vector<Vector2i> &rRegionLocations,
		const AABB &rEditedArea, T3DCompute *pCompute
	);
	void finalizeMapEdits( const T3DMapMutation &rMutation, bool bKeepEdited );
	void recycleMutation( T3DMapMutation &&rMutation );

	const eastl::vector<T3DDerivedMapUpdate> &regenerateDerivedMaps(
		const eastl::vector<Vector2i> &rRegionLocations,
		const AABB &rEditedArea, T3DCompute *pCompute
	);

	bool buildRegionEditArea(
		const Vector2i &rRegionLocation, const Rect2i &rDirtyRect,
		AABB &rEditedArea
	) const;

	void addEditedArea( const AABB &rArea );
	EA_FORCE_INLINE void clearEditedArea( void ) { m_editedArea = AABB(); }
	EA_FORCE_INLINE const AABB &getEditedArea( void ) const { return m_editedArea; }

	EA_FORCE_INLINE void resetHeightRange( void ) { m_masterHeightRange = V2_ZERO; }
	EA_FORCE_INLINE const Vector2 &getHeightRange( void ) const { return m_masterHeightRange; }
	void updateMasterHeight( real_t fHeight );
	void updateMasterHeights( const Vector2 &rLowHigh );
	void calculateHeightRange(
		bool bRecursive = false, EnsureRegionResidentFn pEnsureResident = nullptr,
		void *pResidentContext = nullptr
	);

	void copyRegionMaps(
		const T3DRegion *pSourceRegion, const Rect2i &rSourceRect,
		const Rect2i &rDestinationRect, T3DRegion *pDestinationRegion
	) const;

	Error importImages(
		const TypedArray<Image> &rImages, const Vector3 &rGlobalPosition,
		real_t fOffset, real_t fScale, AcquireEditableRegionFn pAcquireRegion,
		void *pAcquireContext, bool &rGenerateMipmaps
	);

	Ref<Image> layeredToImage(
		t3dmapType_t eMapType, EnsureRegionResidentFn pEnsureResident = nullptr,
		void *pResidentContext = nullptr
	) const;

	size_t getTransientScratchBytes( void ) const;

	static Rect2i getRegionDirtyRect(
		const Vector2i &rLocation, int nRegionSize, real_t fVertexSpacing,
		const AABB &rEditedArea, int nMargin = 0
	);

private:
	T3DRegionRepository *m_pRepository = nullptr;
	T3DSamplingStorage *m_pSamplingStorage = nullptr;

	AABB m_editedArea;
	Vector2 m_masterHeightRange = V2_ZERO;

	// Reused on the synchronous edit path. Worker tasks use private local halos.
	eastl::vector<float> m_fDerivedHaloScratch;
	eastl::vector<float> m_fDerivedWorkerHaloScratch;
	eastl::vector<uint8_t> m_derivedOutputScratch;
	eastl::vector<Vector2i> m_activeLocationScratch;
	eastl::vector<Vector2i> m_derivedLocationScratch;
	eastl::vector<T3DDerivedMapUpdate> m_derivedUpdateScratch;

	T3DMapMutation makeMutationResult( t3dmapType_t eMapType, const AABB &rEditedArea,
		const Vector2 &rEditedHeightRange );
};

#endif // TERRAIN3D_MAP_EDIT_SERVICE_H
