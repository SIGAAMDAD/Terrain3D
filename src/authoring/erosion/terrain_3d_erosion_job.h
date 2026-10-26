// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_EROSION_JOB_CLASS_H
#define TERRAIN3D_EROSION_JOB_CLASS_H

#include "Terrain3DTypes.h"

#include "Terrain3DConstants.h"
#include "core/utility/terrain_3d_sync.h"
#include "authoring/erosion/terrain_3d_erosion_settings.h"
#include "data/formats/terrain_3d_half_buffer.h"
#include "eastl_config.h"
#include "memory/arena/terrain_3d_linear_arena.h"

#include <EASTL/vector.h>
#include <stddef.h>
#include <stdint.h>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include "data/model/terrain_3d_data_access.h"
#include "simd/core/terrain_3d_parallel.h"

typedef struct t3dRegionSnapshot_s
{
	Vector2i		m_location;
	int32_t			m_nSourceOrder = 0;
	uint64_t		m_nSourceHash = 0;
	PackedByteArray m_heightData;
	PackedByteArray m_controlData;
} t3dRegionSnapshot_t;

typedef struct t3dImageSnapshot_s
{
	Vector2i		m_size;
	PackedByteArray m_rfData;

	EA_FORCE_INLINE bool isValid(
		void ) const {
		return m_size.x > 0 && m_size.y > 0 &&
			   m_rfData.size() >= ( int64_t ) ( m_size.x ) * ( int64_t ) ( m_size.y ) * ( int64_t ) ( sizeof( float ) );
	}
	EA_FORCE_INLINE void clear(
		void ) {
		m_size = Vector2i();
		m_rfData.clear();
	}
} t3dImageSnapshot_t;

typedef struct t3dConfig_s : public t3dErosionConfigSnapshot_t
{
	bool					   m_bAutoShaderEnabled = false;
	int32_t					   m_nAutoBaseTexture = 0;
	int32_t					   m_nAutoOverlayTexture = 1;
	float					   m_fAutoSlope = 1.0f;
	float					   m_fAutoHeightReduction = 0.0f;
	t3dTextureErosionProfile_t m_textureProfiles[T3D_EROSION_TEXTURE_PROFILE_COUNT];
	eastl::vector< float >	   m_curveSamples;
	int32_t					   m_nCurveStride = 0;
} t3dConfig_t;

struct t3dErosionApplyBatch_t
{
	const Terrain3DParallelExecutor &m_executor;
	size_t							 m_nSourcePixelCount;
	size_t							 m_nGrain;
	eastl::vector< uint8_t >		 m_height;
	eastl::vector< uint8_t >		 m_control;
	eastl::vector< uint8_t >		 m_color;
	eastl::vector< Vector2i >		 m_changedLocations[REGION_TYPE_MAX];
	bool							 m_bControlChanged = false;
	bool							 m_bColorChanged = false;
};

class Terrain3DErosionJob : public RefCounted
{
	GDCLASS( Terrain3DErosionJob, RefCounted );
	CLASS_NAME();
	friend class Terrain3DData;
	friend struct t3dErosionKernels_s;

private:
	typedef Terrain3DArenaBuffer< float >	FloatBuffer;
	typedef Terrain3DHalfBuffer				HalfBuffer;
	typedef Terrain3DArenaBuffer< uint8_t > ByteBuffer;
	typedef Terrain3DArenaBuffer< int64_t > IndexBuffer;
	struct PlaneBinding
	{
		FloatBuffer *pFloat = NULL;
		HalfBuffer	*pHalf = NULL;
		ByteBuffer	*pByte = NULL;
		IndexBuffer *pIndex = NULL;
		bool		 bEnabled;
		float		 fInitial;
		bool		 bPhase;
		PlaneBinding(
			FloatBuffer &rBuffer,
			bool		 bActive,
			float		 fValue,
			bool		 bScratch ) :
			pFloat( &rBuffer ),
			bEnabled( bActive ),
			fInitial( fValue ),
			bPhase( bScratch ) {
		}
		PlaneBinding(
			HalfBuffer &rBuffer,
			bool		bActive,
			float		fValue,
			bool		bScratch ) :
			pHalf( &rBuffer ),
			bEnabled( bActive ),
			fInitial( fValue ),
			bPhase( bScratch ) {
		}
		PlaneBinding(
			ByteBuffer &rBuffer,
			bool		bActive,
			float		fValue,
			bool		bScratch ) :
			pByte( &rBuffer ),
			bEnabled( bActive ),
			fInitial( fValue ),
			bPhase( bScratch ) {
		}
		PlaneBinding(
			IndexBuffer &rBuffer,
			bool		 bActive,
			float		 fValue,
			bool		 bScratch ) :
			pIndex( &rBuffer ),
			bEnabled( bActive ),
			fInitial( fValue ),
			bPhase( bScratch ) {
		}
	};
	void buildBufferBindings( eastl::vector< PlaneBinding > &rBindings );
	void clearBoundBuffers( const eastl::vector< PlaneBinding > &rBindings );
	bool planBoundBuffers( const eastl::vector< PlaneBinding > &rBindings,
						   Terrain3DLinearArenaLayout		   &rPersistent,
						   Terrain3DLinearArenaLayout		   &rPhase ) const;
	bool assignBoundBuffers( const eastl::vector< PlaneBinding > &rBindings );

	t3dConfig_t							 m_config;
	eastl::vector< t3dRegionSnapshot_t > m_regions;
	eastl::vector< RegionNeighbors >	 m_szRegionNeighbors;
	int16_t								 m_szRegionLookup[T3D_REGION_SLOTS] = {};
	int32_t								 m_nSimulationRegionSize = 0;
	int32_t								 m_nSimulationRegionShift = 0;
	int32_t								 m_nSimulationRegionMask = 0;
	int32_t								 m_nCellsPerRegionShift = 0;
	size_t								 m_nCellsPerRegionMask = 0u;
	int32_t								 m_nResolutionScale = 1;
	float								 m_fVertexSpacing = 1.0f;
	float								 m_fCellSize = 1.0f;
	size_t								 m_nCellsPerRegion = 0;
	size_t								 m_nCellCount = 0;
	Rect2								 m_terrainWorldRect;
	t3dImageSnapshot_t					 m_simulationMaskSnapshot;
	t3dImageSnapshot_t					 m_trafficMaskSnapshot;
	t3dImageSnapshot_t					 m_salinityMaskSnapshot;
	t3dImageSnapshot_t					 m_surfaceLayerDepthSnapshot;
	t3dImageSnapshot_t					 m_bedrockHardnessSnapshot;
	uint64_t							 m_nInputSnapshotBytes = 0;
	uint64_t							 m_nSourceControlSnapshotBytes = 0;

	// The persistent and scratch maps use independent exact-size slabs. Both
	// reject upstream allocations, so the configured budget is enforceable.
	Terrain3DLinearArena m_persistentMemory;
	Terrain3DLinearArena m_phaseMemory;
	Terrain3DLinearArena m_resultMemory;
	Terrain3DLinearArena m_previewMemory;
	bool				 m_bResultsCompacted = false;
	uint64_t			 m_nRetainedResultBytes = 0u;

	ByteBuffer	m_active;
	FloatBuffer m_simulationWeight;
	FloatBuffer m_initialHeight;
	FloatBuffer m_height;
	FloatBuffer m_water;
	FloatBuffer m_sedimentCoarse;
	FloatBuffer m_sedimentSand;
	FloatBuffer m_sedimentFine;
	FloatBuffer m_surfaceCoarse;
	FloatBuffer m_surfaceSand;
	FloatBuffer m_surfaceFine;
	HalfBuffer	m_substrateHardness;
	FloatBuffer m_hardness;
	FloatBuffer m_oxidation;
	FloatBuffer m_flowAccumulation;
	FloatBuffer m_hydraulicDeposition;
	FloatBuffer m_windDeposition;
	FloatBuffer m_waveDeposition;
	FloatBuffer m_weathering;
	FloatBuffer m_traffic;
	FloatBuffer m_compaction;
	FloatBuffer m_alluvialDeposition;
	FloatBuffer m_debrisFlowDeposition;
	FloatBuffer m_duneDeposition;
	FloatBuffer m_saltWeathering;
	FloatBuffer m_wetDryCycles;
	FloatBuffer m_previousMoisture;
	ByteBuffer	m_wetState;
	HalfBuffer	m_salinity;
	FloatBuffer m_snowpack;
	FloatBuffer m_exposureAge;
	FloatBuffer m_desertPavement;
	FloatBuffer m_cumulativeErosion;
	FloatBuffer m_surfaceLayerDepth;
	HalfBuffer	m_bedrockHardness;

	ByteBuffer m_dominantTexture;
	HalfBuffer m_hydraulicErodibility;
	HalfBuffer m_windErodibility;
	HalfBuffer m_waveErodibility;
	HalfBuffer m_weatheringSusceptibility;
	HalfBuffer m_oxidationSusceptibility;
	HalfBuffer m_saltSusceptibility;
	HalfBuffer m_wetDrySusceptibility;
	HalfBuffer m_infiltrationMultiplier;
	HalfBuffer m_trafficErodibility;
	HalfBuffer m_cohesion;
	HalfBuffer m_grainSize;

	ByteBuffer	m_materialTransitionKind;
	FloatBuffer m_materialTransitionAmount;

	// Solver scratch is allocated with the persistent channels in one guarded
	// phase. run() and snapshot construction do not issue additional large heap
	// allocations, and next_a is reused for traffic-mask dilation.
	FloatBuffer m_szFlux[4];
	FloatBuffer m_nextA;
	FloatBuffer m_nextB;
	FloatBuffer m_nextCoarse;
	FloatBuffer m_nextSand;
	FloatBuffer m_nextFine;
	FloatBuffer m_transferAmount;
	FloatBuffer m_transferCoarse;
	FloatBuffer m_transferSand;
	FloatBuffer m_transferFine;
	IndexBuffer m_transferDestination;
	FloatBuffer m_stepHeightStart;
	FloatBuffer m_previewHeight;

	mutable Terrain3DMutex	   m_previewMutex;
	Terrain3DCondition		   m_previewCondition;
	Terrain3DAtomic< int32_t > m_previewRevision{ 0 };
	Terrain3DAtomic< int32_t > m_previewStep{ 0 };
	Terrain3DAtomic< bool >	   m_bPreviewPaused{ false };
	Terrain3DAtomic< bool >	   m_bPreviewWaiting{ false };
	Terrain3DAtomic< int32_t > m_previewStepRequests{ 0 };

	uint64_t m_nAssetProfileHash = 0;

	Terrain3DAtomic< int32_t > m_state{ STATE_NEW };
	Terrain3DAtomic< int32_t > m_stage{ STAGE_IDLE };
	Terrain3DAtomic< float >   m_fProgress{ 0.0f };
	Terrain3DAtomic< bool >	   m_bCancelRequested{ false };
	mutable Terrain3DMutex	   m_statusMutex;
	Terrain3DMutex			   m_applyMutex;
	String					   m_errorMessage;
	double					   m_fTotalEroded = 0.0;
	double					   m_fTotalDeposited = 0.0;
	float					   m_fMaximumLowering = 0.0f;
	float					   m_fMaximumRaising = 0.0f;
	Terrain3DAtomic< int32_t > m_nCompletedSteps{ 0 };

	Error		setup( Terrain3DDataAccess *pData, const Ref< Terrain3DErosionSettings > &rSettings );
	void		copyConfig( const Ref< Terrain3DErosionSettings > &rSettings );
	void		sampleCurves( const Ref< Terrain3DErosionSettings > &rSettings );
	float		sampleCurve( t3dCurveId_t curve, int32_t nStep ) const;
	int32_t		selectResolutionScale( int32_t nRegionCount ) const;
	uint64_t	estimateMemoryBytes( int32_t nRegionCount, int32_t nScale ) const;
	bool		allocateBuffers( void );
	bool		compactCompletedResults( void );
	bool		captureSourceRegion( Terrain3DDataAccess *pData, const Vector2i &rLocation, int32_t nRegionId );
	void		indexSourceRegions( void );
	Error		configureSimulationGrid( Terrain3DDataAccess *pData );
	Error		applyRegionOutputs( Terrain3DDataAccess			 *pData,
									const Ref< Terrain3DRegion > &pRegion,
									int32_t						  nRegionId,
									t3dErosionApplyBatch_t		 &rBatch );
	Error		prepareApplyRegions( Terrain3DDataAccess *pData, eastl::vector< Ref< Terrain3DRegion > > &rRegions );
	bool		captureSourceRegions( Terrain3DDataAccess *pData );
	static bool regionSnapshotLess( const t3dRegionSnapshot_t &rA, const t3dRegionSnapshot_t &rB );
	static float
	sampleImageSnapshot( const t3dImageSnapshot_t &rImage, const Rect2 &rWorldRect, const Vector2 &rWorldPosition );
	bool	 captureImageSnapshot( const Ref< Image > &rImage, t3dImageSnapshot_t &rSnapshot, const char *pszLabel );
	bool	 buildSimulationSnapshot( void );
	void	 releaseSourceSnapshots( void );
	bool	 verifySourceUnchanged( Terrain3DDataAccess *pData ) const;
	uint64_t hashRegionImages( const Ref< Image > &rHeight, const Ref< Image > &rControl ) const;
	int64_t	 neighborIndex( size_t iIndex, int32_t nDx, int32_t nDy ) const;
	uint64_t stochasticIndex( size_t iIndex ) const;
	float	 getSlope( size_t iIndex ) const;
	float	 sampleOutputCell( const FloatBuffer &rMap,
							   int32_t			  nRegionId,
							   int32_t			  iBaseX,
							   int32_t			  iBaseY,
							   int32_t			  iX,
							   int32_t			  iY ) const;
	float	 sampleOutput( const FloatBuffer &rMap, int32_t nRegionId, float fSourceX, float fSourceY ) const;
	bool	 snapshotTextureProfiles( Terrain3DDataAccess *pData );
	uint64_t hashTextureProfiles( Terrain3DDataAccess *pData ) const;
	int32_t	 getTransitionTarget( int32_t nTextureId, t3dMaterialTransitionKind_t kind ) const;
	void	 partitionMaterial( float fGrainSize, float fAmount, float &fCoarse, float &fSand, float &fFine ) const;
	float	 getLooseMaterial( size_t iIndex ) const;
	float	 getSediment( size_t iIndex ) const;
	float	 entrainSurface( size_t iIndex, float fAmount, float &fCoarse, float &fSand, float &fFine );
	void	 depositSurface( size_t iIndex, float fCoarse, float fSand, float fFine );
	void	 buildMaterialTransitions( void );
	void	 setErrorMessage( const String &rMessage );
	void	 setFailed( const String &rMessage );
	void	 setStage( t3dStage_t stage, float fProgress );
	bool	 allocatePreviewBuffer( void );
	void	 publishPreviewSnapshot( int32_t nStep, bool bForce = false );
	bool	 waitForPreviewPermission( void );
	Ref< Image > makeHeightImage( const FloatBuffer &rSource, int32_t nRegionId, bool bFullResolution ) const;

public:
	Terrain3DErosionJob( void );
	~Terrain3DErosionJob( void ) = default;

	void						  run( void );
	void						  cancel( void );
	void						  setPreviewPaused( bool bPaused );
	void						  requestPreviewStep( void );
	Error						  apply( Object *pData, bool bForce = false );
	EA_FORCE_INLINE t3dJobState_t getState(
		void ) const {
		return ( t3dJobState_t ) ( m_state.load() );
	}
	EA_FORCE_INLINE float getProgress(
		void ) const {
		return m_fProgress.load();
	}
	String				 getStageName( void ) const;
	String				 getErrorMessage( void ) const;
	bool				 isComplete( void ) const;
	bool				 isSourceUnchanged( Object *pData ) const;
	EA_FORCE_INLINE bool wasCancelled(
		void ) const {
		return m_state.load() == STATE_CANCELLED;
	}
	EA_FORCE_INLINE int32_t getEffectiveResolutionScale(
		void ) const {
		return m_nResolutionScale;
	}
	EA_FORCE_INLINE int32_t getCompletedSteps(
		void ) const {
		return m_nCompletedSteps.load();
	}
	EA_FORCE_INLINE bool isLivePreviewEnabled(
		void ) const {
		return m_config.m_bLivePreviewEnabled;
	}
	EA_FORCE_INLINE bool isPreviewPaused(
		void ) const {
		return m_bPreviewPaused.load();
	}
	EA_FORCE_INLINE bool isPreviewWaiting(
		void ) const {
		return m_bPreviewWaiting.load();
	}
	EA_FORCE_INLINE int32_t getPreviewRevision(
		void ) const {
		return m_previewRevision.load();
	}
	EA_FORCE_INLINE int32_t getPreviewStep(
		void ) const {
		return m_previewStep.load();
	}
	EA_FORCE_INLINE int32_t getSimulationSteps(
		void ) const {
		return m_config.m_nSimulationSteps;
	}
	EA_FORCE_INLINE int32_t getSimulationRegionSize(
		void ) const {
		return m_nSimulationRegionSize;
	}
	EA_FORCE_INLINE float getPreviewCellSize(
		void ) const {
		return m_fCellSize;
	}
	Dictionary			   getStatistics( void ) const;
	TypedArray< Vector2i > getRegionLocations( void ) const;
	Ref< Image > getInitialRegionHeightMap( const Vector2i &rRegionLocation, bool bFullResolution = false ) const;
	Ref< Image > getPreviewRegionHeightMap( const Vector2i &rRegionLocation, bool bFullResolution = false ) const;
	Ref< Image >
	getRegionOutputMap( const Vector2i &rRegionLocation, t3dOutputMap_t output, bool bFullResolution = false ) const;

protected:
	static void _bind_methods( void );
};

VARIANT_ENUM_CAST( t3dJobState_t );
VARIANT_ENUM_CAST( t3dOutputMap_t );
#endif // TERRAIN3D_EROSION_JOB_CLASS_H
