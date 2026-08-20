// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#pragma once

#ifndef TERRAIN3D_EDITOR_CLASS_H
#define TERRAIN3D_EDITOR_CLASS_H

#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/image_texture.hpp>

#include "eastl_config.h"
#include <EASTL/array.h>
#include <EASTL/vector.h>

#include "terrain_3d.h"
#include "rendering/terrain_3d_compute.h"
#include "data/terrain_3d_region.h"

class T3DSampler;

class Terrain3DEditor : public Object {
	GDCLASS( Terrain3DEditor, Object );
	CLASS_NAME();

public: // Constants
	typedef enum t3dtool_e {
		REGION,
		SCULPT,
		HEIGHT,
		TEXTURE,
		COLOR,
		ROUGHNESS,
		AUTOSHADER,
		HOLES,
		NAVIGATION,
		INSTANCER,
		ANGLE, // used for picking, TODO change to a picking tool
		SCALE, // used for picking
		LANDFORM,
		TOOL_MAX,
	} t3dtool_t;


	typedef enum t3doperation_e {
		ADD,
		SUBTRACT,
		REPLACE,
		AVERAGE,
		GRADIENT,
		OP_MAX,
	} t3doperation_t;


	typedef enum t3daverageMode_e {
		AVG_HEIGHT,
		AVG_BLEND,
		AVG_ROUGHNESS,
	} t3daverageMode_t;

private:
	Terrain3D *m_pTerrain = nullptr;

	// Painter settings & variables
	t3dtool_t m_tool = REGION;
	t3doperation_t m_operation = ADD;
	Dictionary m_brushData;
	Vector3 m_operationPosition = V3_ZERO;
	Vector3 m_operationMovement = V3_ZERO;
	Array m_operationMovementHistory;
	bool m_bIsOperating = false;
	uint64_t m_nLastRegionBoundsError = 0;
	TypedArray<Terrain3DRegion> m_originalRegions; // Queue for undo
	TypedArray<Terrain3DRegion> m_editedRegions;   // Queue for redo
	Array m_originalEditSnapshots;				   // Targeted map/instance snapshots for undo
	Array m_editedEditSnapshots;				   // Targeted map/instance snapshots for redo
	TypedArray<Vector2i> m_addedRemovedLocations; // Queue for added/removed locations
	AABB m_modifiedArea;
	Dictionary m_undoData; // See _get_undo_data for definition
	uint64_t m_lastPenTick = 0;

	// Large brushes revisit the same source brush pixels millions of times. Cache gamma-adjusted
	// alpha values and invalidate the cache whenever brush settings are replaced.
	Image *m_pCachedBrushImage = nullptr;
	Vector2i m_cachedBrushImageSize = V2I_ZERO;
	real_t m_fCachedBrushGamma = -1.f;
	eastl::vector<float> m_fBrushAlphaCache;

	typedef struct t3daxisSample_s {
		real_t fOffset = 0.f;
		real_t fGlobalPosition = 0.f;
		int32_t regionLocation = 0;
		int32_t mapPixel = 0;
		real_t fCenteredUv = 0.f;
	} t3daxisSample_t;

	typedef struct t3dregionMapCache_s {
		Ref<Terrain3DRegion> pRegion;
		Image *pMap = nullptr;
		Image *pControlMap = nullptr;
		PackedByteArray mapData;
		PackedByteArray controlData;
		uint8_t *pMapBytes = nullptr;
		const uint8_t *pControlBytes = nullptr;
		Vector2i location = V2I_ZERO;
		Image::Format mapFormat = Image::FORMAT_MAX;
		bool bMapHasMipmaps = false;
		bool bMapWasImplicit = false;
		bool bDirectAccess = false;
		bool bModified = false;
	} t3dregionMapCache_t;

	typedef struct t3dcomputeWrite_s {
		size_t iRegionIndex = 0u;
		Vector2i mapPixel = V2I_ZERO;
		uint32_t sourceValue = 0u;
		Vector3 worldPosition = V3_ZERO;
	} t3dcomputeWrite_t;

	// Reused for every stamp in one stroke. Ordinary brushes perform no heap
	// allocation after their first stamp; pathological compute capacities are
	// released at stroke end instead of becoming permanent editor memory.
	eastl::vector<t3daxisSample_t> m_xAxisScratch;
	eastl::vector<t3daxisSample_t> m_zAxisScratch;
	eastl::vector<t3dregionMapCache_t> m_regionCacheScratch;
	eastl::vector<T3DCompute::t3dbrushElement_t> m_computeElementScratch;
	eastl::vector<t3dcomputeWrite_t> m_computeWriteScratch;
	eastl::vector<Vector2i> m_modifiedRegionScratch;

	void trimBrushScratch( void );
	void updateBrushAlphaCache( Image *pBrushImage, const Vector2i &rImageSize, const real_t fGamma );
	static bool isBrushSlopeAllowed(
		T3DSampler &rSampler, bool bUnrestricted,
		const Vector2 &rSlopeRange, const Vector3 &rPosition
	);
	static uint32_t packColorRgba8( const Color &rColor );
	static Color unpackColorRgba8( uint32_t nValue );
	static uint32_t readU32( const uint8_t *pBytes, size_t iIndex );
	static Color readMapPixel(
		const t3dregionMapCache_t &rEntry, size_t iIndex, const Vector2i &rPixel,
		t3dmapType_t mapType
	);
	static uint32_t readControlBits( const t3dregionMapCache_t &rEntry, size_t iIndex, const Vector2i &rPixel );
	static void writeMapPixel(
		t3dregionMapCache_t &rEntry, size_t iIndex, const Vector2i &rPixel,
		const Color &clrValue, t3dmapType_t mapType
	);
	static bool locationLess( const Vector2i &rA, const Vector2i &rB );
	Dictionary captureEditSnapshot(
		const Ref<Terrain3DRegion> &rRegion,
		const Dictionary &rTemplate = Dictionary(),
		bool bMapWasImplicit = false
	) const;
	void applyEditSnapshot(
		const Dictionary &rSnapshot, eastl::array<eastl::vector<Vector2i>, Terrain3DRegion::TYPE_MAX> &rMapUpdates,
		bool &bInstancesChanged
	);

	void sendRegionAabb( const Vector2i &rRegionLoc, const Vector2 &rHeightRange = V2_ZERO );
	Ref<Terrain3DRegion> operateRegion( const Vector2i &rRegionLoc );
	void operateMap( const Vector3 &rGlobalPosition, const real_t fCameraDirection );
	t3dmapType_t getMapType( void ) const;
	bool isInBounds( const Point2i &rPixel, const Point2i &rSize ) const;
	Vector2 getUvPosition(
		const Vector3 &rGlobalPosition, const int nRegionSize,
		const real_t fVertexSpacing
	) const;
	Vector2 getRotatedUv( const Vector2 &rUv, const real_t fAngle ) const;
	void storeUndo( void );
	void applyUndo( const Dictionary &rData );
	float average(
		const t3daverageMode_t mode, const Vector3 &rGlobalPosition, const float fBase,
		const float fNanVal = 0.f, bool bAlt = false
	) const;
	Color average( const Vector3 &rGlobalPosition, const Color &clrBase ) const;

public:
	Terrain3DEditor( void ) {}
	~Terrain3DEditor( void ) {}

	void setTerrain( Terrain3D *const pTerrain ) {
		m_pTerrain = pTerrain;
	}
	Terrain3D *getTerrain( void ) const {
		return m_pTerrain;
	}

	void setBrushData( const Dictionary &rData );
	Dictionary getBrushData( void ) const {
		return m_brushData;
	}
	void setTool( const t3dtool_t tool );
	t3dtool_t getTool( void ) const {
		return m_tool;
	}
	void setOperation( const t3doperation_t operation );
	t3doperation_t getOperation( void ) const {
		return m_operation;
	}

	void startOperation( const Vector3 &rGlobalPosition );
	bool isOperating( void ) const {
		return m_bIsOperating;
	}
	void operate( const Vector3 &rGlobalPosition, const real_t fCameraDirection );
	void backupRegion( const Ref<Terrain3DRegion> &rRegion, bool bMapWasImplicit = false );
	void backupRegionFull( const Ref<Terrain3DRegion> &rRegion );
	void stopOperation( void );

protected:
	static void _bind_methods( void );
};

VARIANT_ENUM_CAST( Terrain3DEditor::t3doperation_t );
VARIANT_ENUM_CAST( Terrain3DEditor::t3dtool_t );

// Inline functions

inline t3dmapType_t Terrain3DEditor::getMapType( void ) const {
	switch ( m_tool ) {
		case SCULPT:
		case HEIGHT:
		case INSTANCER:
		case LANDFORM:
			return Terrain3DRegion::TYPE_HEIGHT;
			break;
		case TEXTURE:
		case AUTOSHADER:
		case HOLES:
		case NAVIGATION:
		case ANGLE:
		case SCALE:
			return Terrain3DRegion::TYPE_CONTROL;
			break;
		case COLOR:
		case ROUGHNESS:
			return Terrain3DRegion::TYPE_COLOR;
			break;
		default:
			return Terrain3DRegion::TYPE_MAX;
	}
}

inline bool Terrain3DEditor::isInBounds( const Point2i &rPixel, const Point2i &rSize ) const {
	const bool bPositive = rPixel.x >= 0 && rPixel.y >= 0;
	const bool bLessThanMax = rPixel.x < rSize.x && rPixel.y < rSize.y;
	return bPositive && bLessThanMax;
}

inline Vector2 Terrain3DEditor::getUvPosition(
	const Vector3 &rGlobalPosition, const int nRegionSize,
	const real_t fVertexSpacing
) const {
	const Vector2 descaled_position_2d = Vector2( rGlobalPosition.x, rGlobalPosition.z ) / fVertexSpacing;
	Vector2 region_position = descaled_position_2d / ( real_t ) ( nRegionSize );
	region_position = region_position.floor();
	Vector2 uv_position = ( descaled_position_2d / ( real_t ) ( nRegionSize ) ) - region_position;
	return uv_position;
}

inline Vector2 Terrain3DEditor::getRotatedUv( const Vector2 &rUv, const real_t fAngle ) const {
	const Vector2 rotation_offset = V2( 0.5f );
	Vector2 uv = ( rUv - rotation_offset ).rotated( fAngle ) + rotation_offset;
	return uv.clamp( V2_ZERO, V2( 1.f ) );
}
#endif // TERRAIN3D_EDITOR_CLASS_H
