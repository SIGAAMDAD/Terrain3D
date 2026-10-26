// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_EDITOR_CLASS_H
#define TERRAIN3D_EDITOR_CLASS_H

#if defined( T3D_EDITOR_NATIVE )
#include "rendering/rd/core/terrain_3d_editor_brush_transaction.h"
#endif
#include "Terrain3DTypes.h"
#include "data/model/terrain_3d_data.h"

#include "data/model/terrain_3d_region.h"
#include "eastl_config.h"
#include "rendering/compute/terrain_3d_compute.h"
#include "core/utility/terrain_3d_owner.h"
#include "core/utility/terrain_3d_authoring_hooks.h"

#include <EASTL/vector.h>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/image_texture.hpp>
#include "data/sampling/terrain_3d_sampler.h"

typedef struct t3dRegionMapCache_s
{
	Ref< Terrain3DRegion > m_region;
	Image				  *m_pMap = NULL;
	Image				  *m_pControlMap = NULL;
	PackedByteArray		   m_mapData;
	PackedByteArray		   m_controlData;
	uint8_t				  *m_pMapBytes = NULL;
	const uint8_t		  *m_pControlBytes = NULL;
	Vector2i			   m_location = s_v2iZero;
	Image::Format		   m_mapFormat = Image::FORMAT_MAX;
	bool				   m_bMapHasMipmaps = false;
	bool				   m_bMapWasImplicit = false;
	bool				   m_bDirectAccess = false;
	bool				   m_bModified = false;
	int32_t				   m_nUndoRegionIndex = -1;
} t3dRegionMapCache_t;

// Each ordinary brush stroke retains only its first-write source tiles. A tile
// is 16x16 pixels; 128x128 authored maps therefore need one 64-bit tile mask.
#define T3D_BRUSH_UNDO_TILE_SIZE ( 16u )
#define T3D_BRUSH_UNDO_TILES_PER_AXIS ( T3D_REGION_SIZE / T3D_BRUSH_UNDO_TILE_SIZE )
static_assert( T3D_REGION_SIZE % T3D_BRUSH_UNDO_TILE_SIZE == 0 &&
				   T3D_BRUSH_UNDO_TILES_PER_AXIS * T3D_BRUSH_UNDO_TILES_PER_AXIS <= 64u,
			   "Brush undo tile mask exceeds 64 bits" );

typedef struct t3dBrushUndoTile_s
{
	uint8_t				 m_nTileIndex = 0u;
	PackedByteArray		 m_before;
} t3dBrushUndoTile_t;

typedef struct t3dBrushUndoRegion_s
{
	Vector2i				m_location = s_v2iZero;
	t3dMapType_t		 m_mapType = REGION_TYPE_MAX;
	Image::Format		 m_format = Image::FORMAT_MAX;
	Vector2				m_heightRange = s_v2Zero;
	bool					m_bWasImplicit = false;
	bool					m_bMipmaps = false;
	uint64_t				m_nTouchedMask = 0u;
	eastl::vector< t3dBrushUndoTile_t > m_tiles;
} t3dBrushUndoRegion_t;

// One undo application accumulates edits by map channel.
typedef struct t3dEditorMapUpdates_s
{
	eastl::vector< Vector2i > m_locations[REGION_TYPE_MAX];
} t3dEditorMapUpdates_t;

class Terrain3DEditor : public Object, public Terrain3DAuthoringHooks
{
	GDCLASS( Terrain3DEditor, Object );
	CLASS_NAME();

public: // Constants
private:
	Terrain3DOwner *m_pTerrain = NULL;

	// Painter settings & variables
	t3dTool_t					  m_tool = REGION;
	t3dOperation_t				  m_nOperation = ADD;
	Dictionary					  m_brushData;
	Vector3						  m_operationPosition = s_v3Zero;
	Vector3						  m_operationMovement = s_v3Zero;
	Array						  m_operationMovementHistory;
	bool						  m_bIsOperating = false;
	uint64_t					  m_nLastRegionBoundsError = 0;
	TypedArray< Terrain3DRegion > m_originalRegions;	   // Queue for undo
	TypedArray< Terrain3DRegion > m_editedRegions;		   // Queue for redo
	Array						  m_originalEditSnapshots; // Targeted map/instance snapshots for undo
	Array						  m_editedEditSnapshots;   // Targeted map/instance snapshots for redo
	TypedArray< Vector2i >		  m_addedRemovedLocations; // Queue for added/removed locations
	AABB						  m_modifiedArea;
	Dictionary					  m_undoData; // See _get_undo_data for definition
	uint64_t					  m_nLastPenTick = 0;
	uint64_t m_nBrushToolSelections = 0u;
	uint64_t m_nBrushDabs = 0u;
	uint64_t m_nBrushGetDataCalls = 0u;
	uint64_t m_nBrushSetDataCalls = 0u;
	uint64_t m_nBrushBytesRead = 0u;
	uint64_t m_nBrushBytesPublished = 0u;
	uint64_t m_nBrushPrepareUsec = 0u;
	uint64_t m_nBrushCpuSampleUsec = 0u;
	uint64_t m_nBrushCpuComputeUsec = 0u;
	uint64_t m_nBrushCommitUsec = 0u;
#if defined( T3D_EDITOR_NATIVE )
    typedef struct t3dEditorBrushSourceRegion_s
    {
        Vector2i m_location;
        PackedByteArray m_source;
        uint64_t m_nRevision = 0u;
    } t3dEditorBrushSourceRegion_t;
    eastl::vector< t3dEditorBrushSourceRegion_t > m_editorBrushGpuRegions;
    eastl::vector< t3dEditorBrushTile_t > m_editorBrushGpuTiles;
    uint64_t m_nEditorBrushNextStrokeId = 0u;
    uint32_t m_nEditorBrushGpuSequence = 0u;
    bool m_bEditorBrushGpuRejected = false;
    int32_t m_nEditorBrushGpuMapType = -1;
    Vector2i m_editorBrushGpuMaskSize;
    Image *m_pEditorBrushGpuMaskImage = NULL;
    float m_fEditorBrushGpuMaskGamma = 1.f;
#endif
	uint32_t m_nEditorBrushOwnerGeneration = 1u;
	uint64_t m_nPendingEditorBrushStrokeId = 0u;
	bool m_bPendingEditorBrushSealed = false;

	// Large brushes revisit the same source brush pixels millions of times. Cache gamma-adjusted
	// alpha values and invalidate the cache whenever brush settings are replaced.
	Image				  *m_pCachedBrushImage = NULL;
	Vector2i			   m_cachedBrushImageSize = s_v2iZero;
	real_t				   m_fCachedBrushGamma = -1.f;
	eastl::vector< float > m_fBrushAlphaCache;

	// Reused for every stamp in one stroke. Ordinary brushes perform no heap
	// allocation after their first stamp; pathological compute capacities are
	// released at stroke end instead of becoming permanent editor memory.
	eastl::vector< t3dAxisSample_t >	 m_xAxisScratch;
	eastl::vector< t3dAxisSample_t >	 m_zAxisScratch;
	eastl::vector< t3dRegionMapCache_t > m_regionCacheScratch;
	eastl::vector< t3dBrushElement_t >	 m_computeElementScratch;
	eastl::vector< t3dComputeWrite_t >	 m_computeWriteScratch;
	eastl::vector< Vector2i >			 m_modifiedRegionScratch;
	eastl::vector< t3dBrushUndoRegion_t > m_brushUndoRegions;

	void			trimBrushScratch( void );
	void recordBrushUndoTile( t3dRegionMapCache_t &rEntry, const Vector2i &rPixel, t3dMapType_t mapType );
	void finishBrushUndoTiles( void );
	static PackedByteArray captureUndoTile( Image *pMap, const PackedByteArray *pCachedBytes,
											 uint32_t nTileIndex, Image::Format format );

	void			updateBrushAlphaCache( Image *pBrushImage, const Vector2i &rImageSize, const real_t fGamma );
	static bool		isBrushSlopeAllowed( Terrain3DSampler &rSampler,
										 bool			   bUnrestricted,
										 const Vector2	  &rSlopeRange,
										 const Vector3	  &rPosition );
	static uint32_t packColorRgba8( const Color &rColor );
	static Color	unpackColorRgba8( uint32_t nValue );
	static uint32_t readU32( const uint8_t *pBytes, size_t iIndex );
	static Color
	readMapPixel( const t3dRegionMapCache_t &rEntry, size_t iIndex, const Vector2i &rPixel, t3dMapType_t mapType );
	static uint32_t readControlBits( const t3dRegionMapCache_t &rEntry, size_t iIndex, const Vector2i &rPixel );
	static void		writeMapPixel( t3dRegionMapCache_t &rEntry,
								   size_t				iIndex,
								   const Vector2i	   &rPixel,
								   const Color		   &clrValue,
								   t3dMapType_t			mapType );
	static bool		locationLess( const Vector2i &rA, const Vector2i &rB );
	Dictionary		captureEditSnapshot( const Ref< Terrain3DRegion > &rRegion,
										 const Dictionary			  &rTemplate = Dictionary(),
										 bool						   bMapWasImplicit = false ) const;
	void applyEditSnapshot( const Dictionary &rSnapshot, t3dEditorMapUpdates_t &rMapUpdates, bool &bInstancesChanged, bool &bHoleChanged );
	static bool controlHoleBytesDiffer( const PackedByteArray &rOld, const PackedByteArray &rNew );

	class t3dMapOperationState_t
	{
	public:
		Vector3			   m_globalPosition = s_v3Zero;
		t3dMapType_t	   m_mapType = REGION_TYPE_MAX;
		int				   m_nRegionSize = T3D_REGION_SIZE;
		Vector2i		   m_regionVSize = V2I( T3D_REGION_SIZE );
		Terrain3DData	  *m_pData = NULL;
		Image			  *m_pBrushImage = NULL;
		Vector2i		   m_imageSize = s_v2iZero;
		real_t			   m_fBrushSize = 0.f;
		real_t			   m_fStrength = 0.f;
		real_t			   m_fHeight = 0.f;
		Color			   m_color;
		real_t			   m_fRoughness = 0.f;
		bool			   m_bModifierAlt = false;
		bool			   m_bEnableTexture = false;
		bool			   m_bTextureFilter = false;
		int				   m_nMargin = 0;
		int				   m_nAssetId = 0;
		uint32_t		   m_nAssetIdBits = 0u;
		Vector2			   m_slopeRange = s_v2Zero;
		bool			   m_bEnableAngle = false;
		bool			   m_bDynamicAngle = false;
		real_t			   m_fAngle = 0.f;
		bool			   m_bEnableScale = false;
		real_t			   m_fScale = 0.f;
		real_t			   m_fGamma = 1.f;
		PackedVector3Array m_gradientPoints;
		real_t			   m_fRotation = 0.f;
		AABB			   m_editedArea;
		real_t			   m_fVertexSpacing = 0.f;
		int32_t			   m_nSampleCount = 0;
		int64_t			   m_nTotalSamples = 0;
		bool			   m_bUseAlphaCache = false;
		bool			   m_bUnrestrictedSlope = false;
		Vector2			   m_clampedSlopeRange = s_v2Zero;
		bool			   m_bUseBufferedBrush = false;
		bool			   m_bUseDirectMapAccess = false;
		bool			   m_bHoleChanged = false;
		real_t			   m_fRotationCos = 1.f;
		real_t			   m_fRotationSin = 0.f;
		int32_t			   m_nMinRegionX = 0;
		int32_t			   m_nMaxRegionX = 0;
		int32_t			   m_nMinRegionZ = 0;
		int32_t			   m_nMaxRegionZ = 0;
		int32_t			   m_nRegionCacheWidth = 0;
		int32_t			   m_nRegionCacheHeight = 0;
		uint32_t		   m_nComputeUvRotation = 0u;
		uint32_t		   m_nComputeUvScale = 0u;
	};
#if defined( T3D_EDITOR_NATIVE )
    bool operateGpuTilesCold( const t3dMapOperationState_t &rState );
#endif
	bool prepareMapOperationSamples( t3dMapOperationState_t &rState );
	typedef struct t3dBrushSample_s
	{
		t3dRegionMapCache_t &m_entry;
		size_t				 m_nMapIndex;
		const Vector2i		&m_pixel;
		const Vector2		&m_offset;
		const Vector3		&m_globalPosition;
		Vector3				&m_editedPosition;
		const Color			&m_source;
		Color				&m_destination;
		real_t				 m_fAlpha;
	} t3dBrushSample_t;

	bool applyHeightSample( t3dMapOperationState_t &rState, t3dBrushSample_t &rSample );
	bool applyControlSample( t3dMapOperationState_t &rState,
							 t3dBrushSample_t		&rSample,
							 Terrain3DSampler		&rSampler,
							 real_t					&rAngle );
	bool applyColorSample( t3dMapOperationState_t &rState, t3dBrushSample_t &rSample, Terrain3DSampler &rSampler );
	bool processMapOperationSamples( t3dMapOperationState_t &rState );
	void applyComputedMapOperationSamples( t3dMapOperationState_t &rState );
	void commitMapOperationSamples( t3dMapOperationState_t &rState );
	void sendRegionAabb( const Vector2i &rRegionLoc, const Vector2 &rHeightRange = s_v2Zero );
	Ref< Terrain3DRegion > operateRegion( const Vector2i &rRegionLoc );
	void				   operateMap( const Vector3 &rGlobalPosition, const real_t fCameraDirection );
	t3dMapType_t		   getMapType( void ) const;
	bool				   isInBounds( const Point2i &rPixel, const Point2i &rSize ) const;
	Vector2				   getRotatedUv( const Vector2 &rUv, const real_t fAngle ) const;
	void				   storeUndo( void );
	void				   applyUndo( const Dictionary &rData );
	float				   average( const t3dAverageMode_t mode,
									const Vector3		  &rGlobalPosition,
									const float			   fBase,
									const float			   fNanVal = 0.f,
									bool				   bAlt = false ) const;
	Color				   average( const Vector3 &rGlobalPosition, const Color &clrBase ) const;

public:
	EA_FORCE_INLINE Terrain3DEditor(
		void ) {
	}
	EA_FORCE_INLINE ~Terrain3DEditor(
		void ) {
		setTerrain( NULL );
	}

	void	setTerrain( Object *pTerrain );
	Object *getTerrain( void ) const;

	void					   setBrushData( const Dictionary &rData );
	Dictionary getBrushPerformanceStats( void ) const;
	EA_FORCE_INLINE Dictionary getBrushData(
		void ) const override {
		return m_brushData;
	}
	void					  setTool( const t3dTool_t tool );
	EA_FORCE_INLINE t3dTool_t getTool(
		void ) const {
		return m_tool;
	}
	void						   setOperation( const t3dOperation_t operation );
	EA_FORCE_INLINE t3dOperation_t getOperation(
		void ) const {
		return m_nOperation;
	}

	void				 startOperation( const Vector3 &rGlobalPosition );
	EA_FORCE_INLINE bool isOperating(
		void ) const override {
		return m_bIsOperating;
	}
	void operate( const Vector3 &rGlobalPosition, const real_t fCameraDirection );
	void backupRegion( const Ref< Terrain3DRegion > &rRegion, bool bMapWasImplicit = false ) override;
	void backupRegionFull( const Ref< Terrain3DRegion > &rRegion );
	void stopOperation( void );
#if defined( T3D_EDITOR_NATIVE )
	// Native owner-thread integration points for the future shared-RD producer.
	uint32_t getEditorBrushOwnerGenerationCold( void ) const { return m_nEditorBrushOwnerGeneration; }
	bool publishEditorBrushReadbackCold( uint64_t nStrokeId );
	bool acceptEditorBrushStrokeCold( uint64_t nStrokeId );
	void pollEditorBrushCommitCold( void );
#endif
	void cancelOperation( void );
	void releaseRuntimeAllocatorStorage( void ) override;
	void onTerrainOwnerDetached( void ) override;

protected:
	static void _bind_methods( void );
};

VARIANT_ENUM_CAST( t3dOperation_t );
VARIANT_ENUM_CAST( t3dTool_t );

#endif // TERRAIN3D_EDITOR_CLASS_H
