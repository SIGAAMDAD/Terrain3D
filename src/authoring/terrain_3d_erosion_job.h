// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_EROSION_JOB_CLASS_H
#define TERRAIN3D_EROSION_JOB_CLASS_H

#include "Terrain3DConstants.h"
#include "core/terrain_3d_sync.h"
#include "authoring/terrain_3d_erosion_settings.h"
#include "data/terrain_3d_half_buffer.h"
#include "eastl_config.h"
#include "memory/terrain_3d_linear_arena.h"

#include <EASTL/vector.h>
#include <stddef.h>
#include <stdint.h>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
class Terrain3DData;
typedef struct t3dErosionKernels_s t3dErosionKernels_t;

typedef enum t3dJobState_e {
	STATE_NEW,
	STATE_READY,
	STATE_RUNNING,
	STATE_COMPLETED,
	STATE_CANCELLED,
	STATE_FAILED,
	STATE_APPLIED,
} t3dJobState_t;

typedef enum t3dOutputMap_e {
	OUTPUT_HEIGHT_DELTA,
	OUTPUT_FLOW_ACCUMULATION,
	OUTPUT_SEDIMENT,
	OUTPUT_COARSE_SEDIMENT,
	OUTPUT_SAND_SEDIMENT,
	OUTPUT_FINE_SEDIMENT,
	OUTPUT_LOOSE_MATERIAL,
	OUTPUT_OXIDATION,
	OUTPUT_WEATHERING,
	OUTPUT_HYDRAULIC_DEPOSITION,
	OUTPUT_WIND_DEPOSITION,
	OUTPUT_WAVE_DEPOSITION,
	OUTPUT_TRAFFIC,
	OUTPUT_ALLUVIAL_DEPOSITION,
	OUTPUT_DEBRIS_FLOW_DEPOSITION,
	OUTPUT_DUNE_DEPOSITION,
	OUTPUT_SALT_WEATHERING,
	OUTPUT_WET_DRY_CYCLES,
	OUTPUT_EXPOSURE_AGE,
	OUTPUT_SNOWPACK,
	OUTPUT_DESERT_PAVEMENT,
	OUTPUT_STRATIGRAPHY,
	OUTPUT_MATERIAL_ID,
	OUTPUT_MAX,
} t3dOutputMap_t;

typedef enum t3dMaterialTransitionKind_e : uint8_t {
	TRANSITION_NONE,
	TRANSITION_EXPOSED,
	TRANSITION_WEATHERED,
	TRANSITION_OXIDIZED,
	TRANSITION_COARSE_DEPOSIT,
	TRANSITION_FINE_DEPOSIT,
	TRANSITION_SALT_WEATHERED,
	TRANSITION_DESERT_PAVEMENT,
	TRANSITION_VARNISHED,
} t3dMaterialTransitionKind_t;

typedef enum t3dStage_e {
	STAGE_IDLE,
	STAGE_SNAPSHOT,
	STAGE_WEATHERING,
	STAGE_HYDRAULIC,
	STAGE_DEBRIS_FLOW,
	STAGE_WIND,
	STAGE_DUNES,
	STAGE_WAVES,
	STAGE_TRAFFIC,
	STAGE_MATERIALS,
	STAGE_COMPLETE,
	STAGE_CANCELLED,
	STAGE_FAILED,
} t3dStage_t;

typedef struct t3dRegionSnapshot_s {
	Vector2i m_location;
	int32_t m_nSourceOrder = 0;
	uint64_t m_nSourceHash = 0;
	PackedByteArray m_heightData;
	PackedByteArray m_controlData;
} t3dRegionSnapshot_t;

typedef struct t3dImageSnapshot_s {
	Vector2i m_size;
	PackedByteArray m_rfData;

	EA_FORCE_INLINE bool isValid( void ) const {
		return m_size.x > 0 && m_size.y > 0
		        && m_rfData.size() >= ( int64_t )( m_size.x ) * ( int64_t )( m_size.y ) * ( int64_t )( sizeof( float ) );
	}
	EA_FORCE_INLINE void clear( void ) {
		m_size = Vector2i();
		m_rfData.clear();
	}
} t3dImageSnapshot_t;

typedef struct t3dTextureErosionProfile_s {
	bool m_bValid = false;
	bool m_bEnabled = true;
	float m_fHardness = -1.0f;
	float m_fHydraulicErodibility = 1.0f;
	float m_fWindErodibility = 1.0f;
	float m_fWaveErodibility = 1.0f;
	float m_fWeatheringSusceptibility = 1.0f;
	float m_fOxidationSusceptibility = 1.0f;
	float m_fSaltSusceptibility = 1.0f;
	float m_fWetDrySusceptibility = 1.0f;
	float m_fInfiltrationMultiplier = 1.0f;
	float m_fTrafficErodibility = 1.0f;
	float m_fCohesion = 0.0f;
	float m_fGrainSize = 0.5f;
	int32_t m_nExposedTextureId = -1;
	int32_t m_nWeatheredTextureId = -1;
	int32_t m_nOxidizedTextureId = -1;
	int32_t m_nCoarseDepositTextureId = -1;
	int32_t m_nFineDepositTextureId = -1;
	int32_t m_nSaltWeatheredTextureId = -1;
	int32_t m_nPavementTextureId = -1;
	int32_t m_nVarnishedTextureId = -1;
} t3dTextureErosionProfile_t;

typedef enum t3dCurveId_e : uint8_t {
	CURVE_MASTER,
	CURVE_RAINFALL,
	CURVE_WEATHERING,
	CURVE_OXIDATION,
	CURVE_WIND,
	CURVE_WAVE,
	CURVE_TRAFFIC,
	CURVE_TEMPERATURE,
	CURVE_SEA_LEVEL,
	CURVE_SNOWMELT,
	CURVE_SALT_WEATHERING,
	CURVE_DEBRIS_FLOW,
	CURVE_COUNT,
} t3dCurveId_t;

typedef struct t3dConfig_s : public t3dErosionConfigSnapshot_t {
	bool m_bAutoShaderEnabled = false;
	int32_t m_nAutoBaseTexture = 0;
	int32_t m_nAutoOverlayTexture = 1;
	float m_fAutoSlope = 1.0f;
	float m_fAutoHeightReduction = 0.0f;
	t3dTextureErosionProfile_t m_textureProfiles[s_nErosionTextureProfileCount];
	eastl::vector<float> m_curveSamples;
	int32_t m_nCurveStride = 0;
} t3dConfig_t;

class Terrain3DErosionJob : public RefCounted {
	GDCLASS( Terrain3DErosionJob, RefCounted );
	CLASS_NAME();
	friend class Terrain3DData;
	friend struct t3dErosionKernels_s;

public:
private:
	typedef Terrain3DArenaBuffer<float> FloatBuffer;
	typedef Terrain3DHalfBuffer HalfBuffer;
	typedef Terrain3DArenaBuffer<uint8_t> ByteBuffer;
	typedef Terrain3DArenaBuffer<int64_t> IndexBuffer;
	struct RegionNeighbors {
		int16_t m_ids[s_nErosionDirectionCount]{};
	};

	t3dConfig_t m_config;
	eastl::vector<t3dRegionSnapshot_t> m_regions;
	eastl::vector<RegionNeighbors> m_szRegionNeighbors;
	int16_t m_szRegionLookup[s_nRegionSlots] = {};
	int32_t m_nSimulationRegionSize = 0;
	int32_t m_nSimulationRegionShift = 0;
	int32_t m_nSimulationRegionMask = 0;
	int32_t m_nCellsPerRegionShift = 0;
	size_t m_nCellsPerRegionMask = 0u;
	int32_t m_nResolutionScale = 1;
	float m_fVertexSpacing = 1.0f;
	float m_fCellSize = 1.0f;
	size_t m_nCellsPerRegion = 0;
	size_t m_nCellCount = 0;
	Rect2 m_terrainWorldRect;
	t3dImageSnapshot_t m_simulationMaskSnapshot;
	t3dImageSnapshot_t m_trafficMaskSnapshot;
	t3dImageSnapshot_t m_salinityMaskSnapshot;
	t3dImageSnapshot_t m_surfaceLayerDepthSnapshot;
	t3dImageSnapshot_t m_bedrockHardnessSnapshot;
	uint64_t m_nInputSnapshotBytes = 0;
	uint64_t m_nSourceControlSnapshotBytes = 0;

	// The persistent and scratch maps use independent exact-size slabs. Both
	// reject upstream allocations, so the configured budget is enforceable.
	Terrain3DLinearArena m_persistentMemory;
	Terrain3DLinearArena m_phaseMemory;
	Terrain3DLinearArena m_resultMemory;
	Terrain3DLinearArena m_previewMemory;
	bool m_bResultsCompacted = false;
	uint64_t m_nRetainedResultBytes = 0u;

	ByteBuffer m_active;
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
	HalfBuffer m_substrateHardness;
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
	ByteBuffer m_wetState;
	HalfBuffer m_salinity;
	FloatBuffer m_snowpack;
	FloatBuffer m_exposureAge;
	FloatBuffer m_desertPavement;
	FloatBuffer m_cumulativeErosion;
	FloatBuffer m_surfaceLayerDepth;
	HalfBuffer m_bedrockHardness;

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

	ByteBuffer m_materialTransitionKind;
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

	mutable t3dMutex_t m_previewMutex;
	t3dCondition_t m_previewCondition;
	t3dAtomic_t<int32_t> m_previewRevision{ 0 };
	t3dAtomic_t<int32_t> m_previewStep{ 0 };
	t3dAtomic_t<bool> m_bPreviewPaused{ false };
	t3dAtomic_t<bool> m_bPreviewWaiting{ false };
	t3dAtomic_t<int32_t> m_previewStepRequests{ 0 };

	uint64_t m_nAssetProfileHash = 0;

	t3dAtomic_t<int32_t> m_state{ STATE_NEW };
	t3dAtomic_t<int32_t> m_stage{ STAGE_IDLE };
	t3dAtomic_t<float> m_fProgress{ 0.0f };
	t3dAtomic_t<bool> m_bCancelRequested{ false };
	mutable t3dMutex_t m_statusMutex;
	t3dMutex_t m_applyMutex;
	String m_errorMessage;
	double m_fTotalEroded = 0.0;
	double m_fTotalDeposited = 0.0;
	float m_fMaximumLowering = 0.0f;
	float m_fMaximumRaising = 0.0f;
	t3dAtomic_t<int32_t> m_nCompletedSteps{ 0 };

	Error setup( Terrain3DData *pData, const Ref<Terrain3DErosionSettings> &rSettings );
	void copyConfig( const Ref<Terrain3DErosionSettings> &rSettings );
	void sampleCurves( const Ref<Terrain3DErosionSettings> &rSettings );
	float sampleCurve( t3dCurveId_t curve, int32_t nStep ) const;
	int32_t selectResolutionScale( int32_t nRegionCount ) const;
	uint64_t estimateMemoryBytes( int32_t nRegionCount, int32_t nScale ) const;
	bool allocateBuffers( void );
	bool compactCompletedResults( void );
	bool captureSourceRegions( Terrain3DData *pData );
	static bool regionSnapshotLess( const t3dRegionSnapshot_t &rA, const t3dRegionSnapshot_t &rB );
	static float sampleImageSnapshot( const t3dImageSnapshot_t &rImage, const Rect2 &rWorldRect, const Vector2 &rWorldPosition );
	bool captureImageSnapshot( const Ref<Image> &rImage, t3dImageSnapshot_t &rSnapshot, const char *pszLabel );
	bool buildSimulationSnapshot( void );
	void releaseSourceSnapshots( void );
	bool verifySourceUnchanged( Terrain3DData *pData ) const;
	uint64_t hashRegionImages( const Ref<Image> &rHeight, const Ref<Image> &rControl ) const;
	int64_t neighborIndex( size_t iIndex, int32_t nDx, int32_t nDy ) const;
	uint64_t stochasticIndex( size_t iIndex ) const;
	float getSlope( size_t iIndex ) const;
	float sampleOutputCell( const FloatBuffer &rMap, int32_t nRegionId, int32_t iBaseX, int32_t iBaseY, int32_t iX, int32_t iY ) const;
	float sampleOutput( const FloatBuffer &rMap, int32_t nRegionId, float fSourceX, float fSourceY ) const;
	bool snapshotTextureProfiles( Terrain3DData *pData );
	uint64_t hashTextureProfiles( Terrain3DData *pData ) const;
	int32_t getTransitionTarget( int32_t nTextureId, t3dMaterialTransitionKind_t kind ) const;
	void partitionMaterial( float fGrainSize, float fAmount, float &fCoarse, float &fSand, float &fFine ) const;
	float getLooseMaterial( size_t iIndex ) const;
	float getSediment( size_t iIndex ) const;
	float entrainSurface( size_t iIndex, float fAmount, float &fCoarse, float &fSand, float &fFine );
	void depositSurface( size_t iIndex, float fCoarse, float fSand, float fFine );
	void buildMaterialTransitions( void );
	void setErrorMessage( const String &rMessage );
	void setFailed( const String &rMessage );
	void setStage( t3dStage_t stage, float fProgress );
	bool allocatePreviewBuffer( void );
	void publishPreviewSnapshot( int32_t nStep, bool bForce = false );
	bool waitForPreviewPermission( void );
	Ref<Image> makeHeightImage( const FloatBuffer &rSource, int32_t nRegionId, bool bFullResolution ) const;

public:
	Terrain3DErosionJob( void );
	~Terrain3DErosionJob( void ) = default;

	void run( void );
	void cancel( void );
	void setPreviewPaused( bool bPaused );
	void requestPreviewStep( void );
	Error apply( Terrain3DData *pData, bool bForce = false );
	EA_FORCE_INLINE t3dJobState_t getState( void ) const {
		return ( t3dJobState_t )( m_state.load() );
	}
	EA_FORCE_INLINE float getProgress( void ) const {
		return m_fProgress.load();
	}
	String getStageName( void ) const;
	String getErrorMessage( void ) const;
	bool isComplete( void ) const;
	bool isSourceUnchanged( Terrain3DData *pData ) const;
	EA_FORCE_INLINE bool wasCancelled( void ) const {
		return m_state.load() == STATE_CANCELLED;
	}
	EA_FORCE_INLINE int32_t getEffectiveResolutionScale( void ) const {
		return m_nResolutionScale;
	}
	EA_FORCE_INLINE int32_t getCompletedSteps( void ) const {
		return m_nCompletedSteps.load();
	}
	EA_FORCE_INLINE bool isLivePreviewEnabled( void ) const {
		return m_config.m_bLivePreviewEnabled;
	}
	EA_FORCE_INLINE bool isPreviewPaused( void ) const {
		return m_bPreviewPaused.load();
	}
	EA_FORCE_INLINE bool isPreviewWaiting( void ) const {
		return m_bPreviewWaiting.load();
	}
	EA_FORCE_INLINE int32_t getPreviewRevision( void ) const {
		return m_previewRevision.load();
	}
	EA_FORCE_INLINE int32_t getPreviewStep( void ) const {
		return m_previewStep.load();
	}
	EA_FORCE_INLINE int32_t getSimulationSteps( void ) const {
		return m_config.m_nSimulationSteps;
	}
	EA_FORCE_INLINE int32_t getSimulationRegionSize( void ) const {
		return m_nSimulationRegionSize;
	}
	EA_FORCE_INLINE float getPreviewCellSize( void ) const {
		return m_fCellSize;
	}
	Dictionary getStatistics( void ) const;
	TypedArray<Vector2i> getRegionLocations( void ) const;
	Ref<Image> getInitialRegionHeightMap( const Vector2i &rRegionLocation, bool bFullResolution = false ) const;
	Ref<Image> getPreviewRegionHeightMap( const Vector2i &rRegionLocation, bool bFullResolution = false ) const;
	Ref<Image> getRegionOutputMap( const Vector2i &rRegionLocation, t3dOutputMap_t output, bool bFullResolution = false ) const;

protected:
	static void _bind_methods( void );
};

VARIANT_ENUM_CAST( t3dJobState_t );
VARIANT_ENUM_CAST( t3dOutputMap_t );
#endif // TERRAIN3D_EROSION_JOB_CLASS_H
