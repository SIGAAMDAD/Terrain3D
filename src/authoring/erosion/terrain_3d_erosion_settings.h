// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_EROSION_SETTINGS_CLASS_H
#define TERRAIN3D_EROSION_SETTINGS_CLASS_H

#include "Terrain3DConstants.h"
#include "core/config/terrain_3d_compiler.h"
#include "authoring/erosion/terrain_3d_erosion_config.h"

#include <godot_cpp/classes/curve.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/resource.hpp>


class Terrain3DErosionSettings : public Resource {
	GDCLASS( Terrain3DErosionSettings, Resource );
	CLASS_NAME();

public:
private:
	t3dErosionConfigSnapshot_t m_config;
	Ref<Image> m_simulationMask;
	Ref<Image> m_salinityMask;
	Ref<Image> m_trafficMask;
	Ref<Image> m_surfaceLayerDepthMap;
	Ref<Image> m_bedrockHardnessMap;

	// Curves are sampled on the main thread before simulation starts.
	Ref<Curve> m_masterIntensityCurve;
	Ref<Curve> m_rainfallCurve;
	Ref<Curve> m_weatheringCurve;
	Ref<Curve> m_oxidationCurve;
	Ref<Curve> m_windCurve;
	Ref<Curve> m_waveCurve;
	Ref<Curve> m_trafficCurve;
	Ref<Curve> m_temperatureCurve;
	Ref<Curve> m_seaLevelCurve;
	Ref<Curve> m_snowmeltCurve;
	Ref<Curve> m_saltWeatheringCurve;
	Ref<Curve> m_debrisFlowCurve;

	static Ref<Curve> makeFlatCurve( float fValue, float fMin = 0.0f, float fMax = 2.0f );
	void setCurve( Ref<Curve> &rTargetCurve, const Ref<Curve> &rSourceCurve );

public:
	Terrain3DErosionSettings( void );
	~Terrain3DErosionSettings( void ) = default;

	EA_FORCE_INLINE const t3dErosionConfigSnapshot_t &getConfigSnapshot( void ) const {
		return m_config;
	}

	void setSimulationYears( double fValue );
	EA_FORCE_INLINE double getSimulationYears( void ) const {
		return m_config.m_fSimulationYears;
	}
	void setSimulationSteps( int32_t nValue );
	EA_FORCE_INLINE int32_t getSimulationSteps( void ) const {
		return m_config.m_nSimulationSteps;
	}
	void setResolutionScale( int32_t nValue );
	EA_FORCE_INLINE int32_t getResolutionScale( void ) const {
		return m_config.m_nResolutionScale;
	}
	void setAutoAdjustResolution( bool bValue );
	EA_FORCE_INLINE bool getAutoAdjustResolution( void ) const {
		return m_config.m_bAutoAdjustResolution;
	}
	void setMemoryBudgetMb( int32_t nValue );
	EA_FORCE_INLINE int32_t getMemoryBudgetMb( void ) const {
		return m_config.m_nMemoryBudgetMb;
	}
	void setLivePreviewEnabled( bool bValue );
	EA_FORCE_INLINE bool getLivePreviewEnabled( void ) const {
		return m_config.m_bLivePreviewEnabled;
	}
	void setLivePreviewIntervalSteps( int32_t nValue );
	EA_FORCE_INLINE int32_t getLivePreviewIntervalSteps( void ) const {
		return m_config.m_nLivePreviewIntervalSteps;
	}
	void setLivePreviewStartPaused( bool bValue );
	EA_FORCE_INLINE bool getLivePreviewStartPaused( void ) const {
		return m_config.m_bLivePreviewStartPaused;
	}
	void setThreadCount( int32_t nValue );
	EA_FORCE_INLINE int32_t getThreadCount( void ) const {
		return m_config.m_nThreadCount;
	}
	void setSeed( int64_t nValue );
	EA_FORCE_INLINE int64_t getSeed( void ) const {
		return m_config.m_nSeed;
	}
	void setBoundaryMode( t3dBoundaryMode_t eValue );
	EA_FORCE_INLINE t3dBoundaryMode_t getBoundaryMode( void ) const {
		return m_config.m_boundaryMode;
	}
	void setMaxHeightChangePerStep( float fValue );
	EA_FORCE_INLINE float getMaxHeightChangePerStep( void ) const {
		return m_config.m_fMaxHeightChangePerStep;
	}
	void setPreserveHoles( bool bValue );
	EA_FORCE_INLINE bool getPreserveHoles( void ) const {
		return m_config.m_bPreserveHoles;
	}
	void setApplyHeight( bool bValue );
	EA_FORCE_INLINE bool getApplyHeight( void ) const {
		return m_config.m_bApplyHeight;
	}
	void setHeightOutputStrength( float fValue );
	EA_FORCE_INLINE float getHeightOutputStrength( void ) const {
		return m_config.m_fHeightOutputStrength;
	}
	void setGlobalIntensity( float fValue );
	EA_FORCE_INLINE float getGlobalIntensity( void ) const {
		return m_config.m_fGlobalIntensity;
	}
	void setSimulationMask( const Ref<Image> &rValue );
	EA_FORCE_INLINE Ref<Image> getSimulationMask( void ) const {
		return m_simulationMask;
	}
	void setSimulationMaskWorldRect( const Rect2 &rValue );
	EA_FORCE_INLINE Rect2 getSimulationMaskWorldRect( void ) const {
		return m_config.m_simulationMaskWorldRect;
	}
	void setInvertSimulationMask( bool bValue );
	EA_FORCE_INLINE bool getInvertSimulationMask( void ) const {
		return m_config.m_bInvertSimulationMask;
	}
	void setEdgeFadeDistanceM( float fValue );
	EA_FORCE_INLINE float getEdgeFadeDistanceM( void ) const {
		return m_config.m_fEdgeFadeDistanceM;
	}
	void setHeightRangeEnabled( bool bValue );
	EA_FORCE_INLINE bool getHeightRangeEnabled( void ) const {
		return m_config.m_bHeightRangeEnabled;
	}
	void setMinimumHeightM( float fValue );
	EA_FORCE_INLINE float getMinimumHeightM( void ) const {
		return m_config.m_fMinimumHeightM;
	}
	void setMaximumHeightM( float fValue );
	EA_FORCE_INLINE float getMaximumHeightM( void ) const {
		return m_config.m_fMaximumHeightM;
	}
	void setHeightRangeFalloffM( float fValue );
	EA_FORCE_INLINE float getHeightRangeFalloffM( void ) const {
		return m_config.m_fHeightRangeFalloffM;
	}

	void setHydraulicEnabled( bool bValue );
	EA_FORCE_INLINE bool getHydraulicEnabled( void ) const {
		return m_config.m_bHydraulicEnabled;
	}
	void setRainfallMPerYear( float fValue );
	EA_FORCE_INLINE float getRainfallMPerYear( void ) const {
		return m_config.m_fRainfallMPerYear;
	}
	void setRainfallVariation( float fValue );
	EA_FORCE_INLINE float getRainfallVariation( void ) const {
		return m_config.m_fRainfallVariation;
	}
	void setEvaporationFraction( float fValue );
	EA_FORCE_INLINE float getEvaporationFraction( void ) const {
		return m_config.m_fEvaporationFraction;
	}
	void setInfiltrationFraction( float fValue );
	EA_FORCE_INLINE float getInfiltrationFraction( void ) const {
		return m_config.m_fInfiltrationFraction;
	}
	void setFlowRate( float fValue );
	EA_FORCE_INLINE float getFlowRate( void ) const {
		return m_config.m_fFlowRate;
	}
	void setSedimentCapacity( float fValue );
	EA_FORCE_INLINE float getSedimentCapacity( void ) const {
		return m_config.m_fSedimentCapacity;
	}
	void setHydraulicErosionMPerYear( float fValue );
	EA_FORCE_INLINE float getHydraulicErosionMPerYear( void ) const {
		return m_config.m_fHydraulicErosionMPerYear;
	}
	void setHydraulicDepositionRate( float fValue );
	EA_FORCE_INLINE float getHydraulicDepositionRate( void ) const {
		return m_config.m_fHydraulicDepositionRate;
	}
	void setMinimumSlope( float fValue );
	EA_FORCE_INLINE float getMinimumSlope( void ) const {
		return m_config.m_fMinimumSlope;
	}
	void setStormFrequency( float fValue );
	EA_FORCE_INLINE float getStormFrequency( void ) const {
		return m_config.m_fStormFrequency;
	}
	void setStormIntensity( float fValue );
	EA_FORCE_INLINE float getStormIntensity( void ) const {
		return m_config.m_fStormIntensity;
	}
	void setChannelIncisionStrength( float fValue );
	EA_FORCE_INLINE float getChannelIncisionStrength( void ) const {
		return m_config.m_fChannelIncisionStrength;
	}
	void setStandingWaterDepositionRate( float fValue );
	EA_FORCE_INLINE float getStandingWaterDepositionRate( void ) const {
		return m_config.m_fStandingWaterDepositionRate;
	}
	void setSedimentSortingEnabled( bool bValue );
	EA_FORCE_INLINE bool getSedimentSortingEnabled( void ) const {
		return m_config.m_bSedimentSortingEnabled;
	}
	void setCoarseTransportMultiplier( float fValue );
	EA_FORCE_INLINE float getCoarseTransportMultiplier( void ) const {
		return m_config.m_fCoarseTransportMultiplier;
	}
	void setSandTransportMultiplier( float fValue );
	EA_FORCE_INLINE float getSandTransportMultiplier( void ) const {
		return m_config.m_fSandTransportMultiplier;
	}
	void setFineTransportMultiplier( float fValue );
	EA_FORCE_INLINE float getFineTransportMultiplier( void ) const {
		return m_config.m_fFineTransportMultiplier;
	}
	void setCoarseSettlingRate( float fValue );
	EA_FORCE_INLINE float getCoarseSettlingRate( void ) const {
		return m_config.m_fCoarseSettlingRate;
	}
	void setSandSettlingRate( float fValue );
	EA_FORCE_INLINE float getSandSettlingRate( void ) const {
		return m_config.m_fSandSettlingRate;
	}
	void setFineSettlingRate( float fValue );
	EA_FORCE_INLINE float getFineSettlingRate( void ) const {
		return m_config.m_fFineSettlingRate;
	}
	void setAlluvialFanEnabled( bool bValue );
	EA_FORCE_INLINE bool getAlluvialFanEnabled( void ) const {
		return m_config.m_bAlluvialFanEnabled;
	}
	void setAlluvialFanStrength( float fValue );
	EA_FORCE_INLINE float getAlluvialFanStrength( void ) const {
		return m_config.m_fAlluvialFanStrength;
	}
	void setAlluvialFanSlopeDrop( float fValue );
	EA_FORCE_INLINE float getAlluvialFanSlopeDrop( void ) const {
		return m_config.m_fAlluvialFanSlopeDrop;
	}
	void setAlluvialFanSpread( float fValue );
	EA_FORCE_INLINE float getAlluvialFanSpread( void ) const {
		return m_config.m_fAlluvialFanSpread;
	}
	void setDebrisFlowEnabled( bool bValue );
	EA_FORCE_INLINE bool getDebrisFlowEnabled( void ) const {
		return m_config.m_bDebrisFlowEnabled;
	}
	void setDebrisFlowConcentrationThreshold( float fValue );
	EA_FORCE_INLINE float getDebrisFlowConcentrationThreshold( void ) const {
		return m_config.m_fDebrisFlowConcentrationThreshold;
	}
	void setDebrisFlowSlopeThreshold( float fValue );
	EA_FORCE_INLINE float getDebrisFlowSlopeThreshold( void ) const {
		return m_config.m_fDebrisFlowSlopeThreshold;
	}
	void setDebrisFlowRate( float fValue );
	EA_FORCE_INLINE float getDebrisFlowRate( void ) const {
		return m_config.m_fDebrisFlowRate;
	}
	void setDebrisFlowDepositionRate( float fValue );
	EA_FORCE_INLINE float getDebrisFlowDepositionRate( void ) const {
		return m_config.m_fDebrisFlowDepositionRate;
	}
	void setDebrisFlowRunoutCells( int32_t nValue );
	EA_FORCE_INLINE int32_t getDebrisFlowRunoutCells( void ) const {
		return m_config.m_nDebrisFlowRunoutCells;
	}

	void setWeatheringEnabled( bool bValue );
	EA_FORCE_INLINE bool getWeatheringEnabled( void ) const {
		return m_config.m_bWeatheringEnabled;
	}
	void setPhysicalWeatheringMPerYear( float fValue );
	EA_FORCE_INLINE float getPhysicalWeatheringMPerYear( void ) const {
		return m_config.m_fPhysicalWeatheringMPerYear;
	}
	void setChemicalWeatheringMPerYear( float fValue );
	EA_FORCE_INLINE float getChemicalWeatheringMPerYear( void ) const {
		return m_config.m_fChemicalWeatheringMPerYear;
	}
	void setFreezeThawStrength( float fValue );
	EA_FORCE_INLINE float getFreezeThawStrength( void ) const {
		return m_config.m_fFreezeThawStrength;
	}
	void setTalusAngleDegrees( float fValue );
	EA_FORCE_INLINE float getTalusAngleDegrees( void ) const {
		return m_config.m_fTalusAngleDegrees;
	}
	void setThermalCreepRate( float fValue );
	EA_FORCE_INLINE float getThermalCreepRate( void ) const {
		return m_config.m_fThermalCreepRate;
	}
	void setBaseHardness( float fValue );
	EA_FORCE_INLINE float getBaseHardness( void ) const {
		return m_config.m_fBaseHardness;
	}
	void setHardnessVariation( float fValue );
	EA_FORCE_INLINE float getHardnessVariation( void ) const {
		return m_config.m_fHardnessVariation;
	}
	void setOxidationEnabled( bool bValue );
	EA_FORCE_INLINE bool getOxidationEnabled( void ) const {
		return m_config.m_bOxidationEnabled;
	}
	void setOxidationRatePerYear( float fValue );
	EA_FORCE_INLINE float getOxidationRatePerYear( void ) const {
		return m_config.m_fOxidationRatePerYear;
	}
	void setOxidationWeakening( float fValue );
	EA_FORCE_INLINE float getOxidationWeakening( void ) const {
		return m_config.m_fOxidationWeakening;
	}
	void setWeatheringSlopeBias( float fValue );
	EA_FORCE_INLINE float getWeatheringSlopeBias( void ) const {
		return m_config.m_fWeatheringSlopeBias;
	}
	void setChemicalMoistureExponent( float fValue );
	EA_FORCE_INLINE float getChemicalMoistureExponent( void ) const {
		return m_config.m_fChemicalMoistureExponent;
	}
	void setFreezePoint( float fValue );
	EA_FORCE_INLINE float getFreezePoint( void ) const {
		return m_config.m_fFreezePoint;
	}
	void setWetDryEnabled( bool bValue );
	EA_FORCE_INLINE bool getWetDryEnabled( void ) const {
		return m_config.m_bWetDryEnabled;
	}
	void setWetDryWeatheringMPerCycle( float fValue );
	EA_FORCE_INLINE float getWetDryWeatheringMPerCycle( void ) const {
		return m_config.m_fWetDryWeatheringMPerCycle;
	}
	void setWetThreshold( float fValue );
	EA_FORCE_INLINE float getWetThreshold( void ) const {
		return m_config.m_fWetThreshold;
	}
	void setDryThreshold( float fValue );
	EA_FORCE_INLINE float getDryThreshold( void ) const {
		return m_config.m_fDryThreshold;
	}
	void setSaltWeatheringEnabled( bool bValue );
	EA_FORCE_INLINE bool getSaltWeatheringEnabled( void ) const {
		return m_config.m_bSaltWeatheringEnabled;
	}
	void setSalinityMask( const Ref<Image> &rValue );
	EA_FORCE_INLINE Ref<Image> getSalinityMask( void ) const {
		return m_salinityMask;
	}
	void setSalinityWorldRect( const Rect2 &rValue );
	EA_FORCE_INLINE Rect2 getSalinityWorldRect( void ) const {
		return m_config.m_salinityWorldRect;
	}
	void setBackgroundSalinity( float fValue );
	EA_FORCE_INLINE float getBackgroundSalinity( void ) const {
		return m_config.m_fBackgroundSalinity;
	}
	void setSaltWeatheringMPerYear( float fValue );
	EA_FORCE_INLINE float getSaltWeatheringMPerYear( void ) const {
		return m_config.m_fSaltWeatheringMPerYear;
	}
	void setSaltCrystallizationStrength( float fValue );
	EA_FORCE_INLINE float getSaltCrystallizationStrength( void ) const {
		return m_config.m_fSaltCrystallizationStrength;
	}
	void setSaltEvaporationBias( float fValue );
	EA_FORCE_INLINE float getSaltEvaporationBias( void ) const {
		return m_config.m_fSaltEvaporationBias;
	}
	void setSaltCohesionLoss( float fValue );
	EA_FORCE_INLINE float getSaltCohesionLoss( void ) const {
		return m_config.m_fSaltCohesionLoss;
	}
	void setExposureAgeEnabled( bool bValue );
	EA_FORCE_INLINE bool getExposureAgeEnabled( void ) const {
		return m_config.m_bExposureAgeEnabled;
	}
	void setVarnishMaturationYears( float fValue );
	EA_FORCE_INLINE float getVarnishMaturationYears( void ) const {
		return m_config.m_fVarnishMaturationYears;
	}
	void setExposureResetDepthM( float fValue );
	EA_FORCE_INLINE float getExposureResetDepthM( void ) const {
		return m_config.m_fExposureResetDepthM;
	}
	void setVarnishStabilitySlope( float fValue );
	EA_FORCE_INLINE float getVarnishStabilitySlope( void ) const {
		return m_config.m_fVarnishStabilitySlope;
	}
	void setVarnishOxidationMultiplier( float fValue );
	EA_FORCE_INLINE float getVarnishOxidationMultiplier( void ) const {
		return m_config.m_fVarnishOxidationMultiplier;
	}
	void setSnowmeltEnabled( bool bValue );
	EA_FORCE_INLINE bool getSnowmeltEnabled( void ) const {
		return m_config.m_bSnowmeltEnabled;
	}
	void setSnowfallFraction( float fValue );
	EA_FORCE_INLINE float getSnowfallFraction( void ) const {
		return m_config.m_fSnowfallFraction;
	}
	void setSnowAccumulationTemperature( float fValue );
	EA_FORCE_INLINE float getSnowAccumulationTemperature( void ) const {
		return m_config.m_fSnowAccumulationTemperature;
	}
	void setSnowMeltTemperature( float fValue );
	EA_FORCE_INLINE float getSnowMeltTemperature( void ) const {
		return m_config.m_fSnowMeltTemperature;
	}
	void setSnowMeltFractionPerStep( float fValue );
	EA_FORCE_INLINE float getSnowMeltFractionPerStep( void ) const {
		return m_config.m_fSnowMeltFractionPerStep;
	}
	void setSnowSublimationFraction( float fValue );
	EA_FORCE_INLINE float getSnowSublimationFraction( void ) const {
		return m_config.m_fSnowSublimationFraction;
	}

	void setWindEnabled( bool bValue );
	EA_FORCE_INLINE bool getWindEnabled( void ) const {
		return m_config.m_bWindEnabled;
	}
	void setWindDirection( const Vector2 &vValue );
	EA_FORCE_INLINE Vector2 getWindDirection( void ) const {
		return m_config.m_windDirection;
	}
	void setWindStrength( float fValue );
	EA_FORCE_INLINE float getWindStrength( void ) const {
		return m_config.m_fWindStrength;
	}
	void setWindVariation( float fValue );
	EA_FORCE_INLINE float getWindVariation( void ) const {
		return m_config.m_fWindVariation;
	}
	void setWindErosionMPerYear( float fValue );
	EA_FORCE_INLINE float getWindErosionMPerYear( void ) const {
		return m_config.m_fWindErosionMPerYear;
	}
	void setWindTransportRate( float fValue );
	EA_FORCE_INLINE float getWindTransportRate( void ) const {
		return m_config.m_fWindTransportRate;
	}
	void setWindDepositionRate( float fValue );
	EA_FORCE_INLINE float getWindDepositionRate( void ) const {
		return m_config.m_fWindDepositionRate;
	}
	void setWindShelterStrength( float fValue );
	EA_FORCE_INLINE float getWindShelterStrength( void ) const {
		return m_config.m_fWindShelterStrength;
	}
	void setWindThreshold( float fValue );
	EA_FORCE_INLINE float getWindThreshold( void ) const {
		return m_config.m_fWindThreshold;
	}
	void setWindTransportDistanceCells( int32_t nValue );
	EA_FORCE_INLINE int32_t getWindTransportDistanceCells( void ) const {
		return m_config.m_nWindTransportDistanceCells;
	}
	void setWindDepositionSlopeBias( float fValue );
	EA_FORCE_INLINE float getWindDepositionSlopeBias( void ) const {
		return m_config.m_fWindDepositionSlopeBias;
	}
	void setDuneEnabled( bool bValue );
	EA_FORCE_INLINE bool getDuneEnabled( void ) const {
		return m_config.m_bDuneEnabled;
	}
	void setSandReposeAngleDegrees( float fValue );
	EA_FORCE_INLINE float getSandReposeAngleDegrees( void ) const {
		return m_config.m_fSandReposeAngleDegrees;
	}
	void setDuneAvalancheRate( float fValue );
	EA_FORCE_INLINE float getDuneAvalancheRate( void ) const {
		return m_config.m_fDuneAvalancheRate;
	}
	void setDuneRelaxationIterations( int32_t nValue );
	EA_FORCE_INLINE int32_t getDuneRelaxationIterations( void ) const {
		return m_config.m_nDuneRelaxationIterations;
	}
	void setDuneMinSandDepthM( float fValue );
	EA_FORCE_INLINE float getDuneMinSandDepthM( void ) const {
		return m_config.m_fDuneMinSandDepthM;
	}
	void setDesertPavementEnabled( bool bValue );
	EA_FORCE_INLINE bool getDesertPavementEnabled( void ) const {
		return m_config.m_bDesertPavementEnabled;
	}
	void setPavementFormationRate( float fValue );
	EA_FORCE_INLINE float getPavementFormationRate( void ) const {
		return m_config.m_fPavementFormationRate;
	}
	void setPavementFineRemovalThreshold( float fValue );
	EA_FORCE_INLINE float getPavementFineRemovalThreshold( void ) const {
		return m_config.m_fPavementFineRemovalThreshold;
	}
	void setPavementArmorStrength( float fValue );
	EA_FORCE_INLINE float getPavementArmorStrength( void ) const {
		return m_config.m_fPavementArmorStrength;
	}
	void setPavementInfiltrationReduction( float fValue );
	EA_FORCE_INLINE float getPavementInfiltrationReduction( void ) const {
		return m_config.m_fPavementInfiltrationReduction;
	}

	void setWaveEnabled( bool bValue );
	EA_FORCE_INLINE bool getWaveEnabled( void ) const {
		return m_config.m_bWaveEnabled;
	}
	void setSeaLevel( float fValue );
	EA_FORCE_INLINE float getSeaLevel( void ) const {
		return m_config.m_fSeaLevel;
	}
	void setTidalRange( float fValue );
	EA_FORCE_INLINE float getTidalRange( void ) const {
		return m_config.m_fTidalRange;
	}
	void setWaveDirection( const Vector2 &vValue );
	EA_FORCE_INLINE Vector2 getWaveDirection( void ) const {
		return m_config.m_waveDirection;
	}
	void setWaveStrength( float fValue );
	EA_FORCE_INLINE float getWaveStrength( void ) const {
		return m_config.m_fWaveStrength;
	}
	void setWaveErosionMPerYear( float fValue );
	EA_FORCE_INLINE float getWaveErosionMPerYear( void ) const {
		return m_config.m_fWaveErosionMPerYear;
	}
	void setWaveDepositionRate( float fValue );
	EA_FORCE_INLINE float getWaveDepositionRate( void ) const {
		return m_config.m_fWaveDepositionRate;
	}
	void setCoastlineBandM( float fValue );
	EA_FORCE_INLINE float getCoastlineBandM( void ) const {
		return m_config.m_fCoastlineBandM;
	}
	void setWaveFetchDistanceM( float fValue );
	EA_FORCE_INLINE float getWaveFetchDistanceM( void ) const {
		return m_config.m_fWaveFetchDistanceM;
	}
	void setWaveUndercutBias( float fValue );
	EA_FORCE_INLINE float getWaveUndercutBias( void ) const {
		return m_config.m_fWaveUndercutBias;
	}
	void setBeachSmoothing( float fValue );
	EA_FORCE_INLINE float getBeachSmoothing( void ) const {
		return m_config.m_fBeachSmoothing;
	}

	void setTrafficEnabled( bool bValue );
	EA_FORCE_INLINE bool getTrafficEnabled( void ) const {
		return m_config.m_bTrafficEnabled;
	}
	void setTrafficMask( const Ref<Image> &rValue );
	EA_FORCE_INLINE Ref<Image> getTrafficMask( void ) const {
		return m_trafficMask;
	}
	void setTrafficWorldRect( const Rect2 &rValue );
	EA_FORCE_INLINE Rect2 getTrafficWorldRect( void ) const {
		return m_config.m_trafficWorldRect;
	}
	void setTrafficErosionMPerYear( float fValue );
	EA_FORCE_INLINE float getTrafficErosionMPerYear( void ) const {
		return m_config.m_fTrafficErosionMPerYear;
	}
	void setTrafficCompaction( float fValue );
	EA_FORCE_INLINE float getTrafficCompaction( void ) const {
		return m_config.m_fTrafficCompaction;
	}
	void setTrafficSmoothing( float fValue );
	EA_FORCE_INLINE float getTrafficSmoothing( void ) const {
		return m_config.m_fTrafficSmoothing;
	}
	void setTrafficRunoffMultiplier( float fValue );
	EA_FORCE_INLINE float getTrafficRunoffMultiplier( void ) const {
		return m_config.m_fTrafficRunoffMultiplier;
	}
	void setTrafficThreshold( float fValue );
	EA_FORCE_INLINE float getTrafficThreshold( void ) const {
		return m_config.m_fTrafficThreshold;
	}
	void setTrafficWideningCells( int32_t nValue );
	EA_FORCE_INLINE int32_t getTrafficWideningCells( void ) const {
		return m_config.m_nTrafficWideningCells;
	}

	void setStratigraphyEnabled( bool bValue );
	EA_FORCE_INLINE bool getStratigraphyEnabled( void ) const {
		return m_config.m_bStratigraphyEnabled;
	}
	void setSurfaceLayerDepthM( float fValue );
	EA_FORCE_INLINE float getSurfaceLayerDepthM( void ) const {
		return m_config.m_fSurfaceLayerDepthM;
	}
	void setSurfaceLayerDepthMap( const Ref<Image> &rValue );
	EA_FORCE_INLINE Ref<Image> getSurfaceLayerDepthMap( void ) const {
		return m_surfaceLayerDepthMap;
	}
	void setSurfaceLayerDepthWorldRect( const Rect2 &rValue );
	EA_FORCE_INLINE Rect2 getSurfaceLayerDepthWorldRect( void ) const {
		return m_config.m_surfaceLayerDepthWorldRect;
	}
	void setBedrockHardness( float fValue );
	EA_FORCE_INLINE float getBedrockHardness( void ) const {
		return m_config.m_fBedrockHardness;
	}
	void setBedrockHardnessMap( const Ref<Image> &rValue );
	EA_FORCE_INLINE Ref<Image> getBedrockHardnessMap( void ) const {
		return m_bedrockHardnessMap;
	}
	void setBedrockHardnessWorldRect( const Rect2 &rValue );
	EA_FORCE_INLINE Rect2 getBedrockHardnessWorldRect( void ) const {
		return m_config.m_bedrockHardnessWorldRect;
	}
	void setStratigraphyTransitionWidthM( float fValue );
	EA_FORCE_INLINE float getStratigraphyTransitionWidthM( void ) const {
		return m_config.m_fStratigraphyTransitionWidthM;
	}

	void setMaterialOutputEnabled( bool bValue );
	EA_FORCE_INLINE bool getMaterialOutputEnabled( void ) const {
		return m_config.m_bMaterialOutputEnabled;
	}
	void setPreserveControlFlags( bool bValue );
	EA_FORCE_INLINE bool getPreserveControlFlags( void ) const {
		return m_config.m_bPreserveControlFlags;
	}
	void setResolveAutoMaterials( bool bValue );
	EA_FORCE_INLINE bool getResolveAutoMaterials( void ) const {
		return m_config.m_bResolveAutoMaterials;
	}
	void setMaterialTransitionStrength( float fValue );
	EA_FORCE_INLINE float getMaterialTransitionStrength( void ) const {
		return m_config.m_fMaterialTransitionStrength;
	}
	void setMaterialTransitionThreshold( float fValue );
	EA_FORCE_INLINE float getMaterialTransitionThreshold( void ) const {
		return m_config.m_fMaterialTransitionThreshold;
	}
	void setMaterialTransitionDepthM( float fValue );
	EA_FORCE_INLINE float getMaterialTransitionDepthM( void ) const {
		return m_config.m_fMaterialTransitionDepthM;
	}
	void setOxidationColorEnabled( bool bValue );
	EA_FORCE_INLINE bool getOxidationColorEnabled( void ) const {
		return m_config.m_bOxidationColorEnabled;
	}
	void setOxidationTint( const Color &clrValue );
	EA_FORCE_INLINE Color getOxidationTint( void ) const {
		return m_config.m_oxidationTint;
	}
	void setOxidationTintStrength( float fValue );
	EA_FORCE_INLINE float getOxidationTintStrength( void ) const {
		return m_config.m_fOxidationTintStrength;
	}

	void setMasterIntensityCurve( const Ref<Curve> &rValue );
	EA_FORCE_INLINE Ref<Curve> getMasterIntensityCurve( void ) const {
		return m_masterIntensityCurve;
	}
	void setRainfallCurve( const Ref<Curve> &rValue );
	EA_FORCE_INLINE Ref<Curve> getRainfallCurve( void ) const {
		return m_rainfallCurve;
	}
	void setWeatheringCurve( const Ref<Curve> &rValue );
	EA_FORCE_INLINE Ref<Curve> getWeatheringCurve( void ) const {
		return m_weatheringCurve;
	}
	void setOxidationCurve( const Ref<Curve> &rValue );
	EA_FORCE_INLINE Ref<Curve> getOxidationCurve( void ) const {
		return m_oxidationCurve;
	}
	void setWindCurve( const Ref<Curve> &rValue );
	EA_FORCE_INLINE Ref<Curve> getWindCurve( void ) const {
		return m_windCurve;
	}
	void setWaveCurve( const Ref<Curve> &rValue );
	EA_FORCE_INLINE Ref<Curve> getWaveCurve( void ) const {
		return m_waveCurve;
	}
	void setTrafficCurve( const Ref<Curve> &rValue );
	EA_FORCE_INLINE Ref<Curve> getTrafficCurve( void ) const {
		return m_trafficCurve;
	}
	void setTemperatureCurve( const Ref<Curve> &rValue );
	EA_FORCE_INLINE Ref<Curve> getTemperatureCurve( void ) const {
		return m_temperatureCurve;
	}
	void setSeaLevelCurve( const Ref<Curve> &rValue );
	EA_FORCE_INLINE Ref<Curve> getSeaLevelCurve( void ) const {
		return m_seaLevelCurve;
	}
	void setSnowmeltCurve( const Ref<Curve> &rValue );
	EA_FORCE_INLINE Ref<Curve> getSnowmeltCurve( void ) const {
		return m_snowmeltCurve;
	}
	void setSaltWeatheringCurve( const Ref<Curve> &rValue );
	EA_FORCE_INLINE Ref<Curve> getSaltWeatheringCurve( void ) const {
		return m_saltWeatheringCurve;
	}
	void setDebrisFlowCurve( const Ref<Curve> &rValue );
	EA_FORCE_INLINE Ref<Curve> getDebrisFlowCurve( void ) const {
		return m_debrisFlowCurve;
	}

protected:
	void _validate_property( PropertyInfo &rProperty ) const;
	static void _bind_methods( void );
};

VARIANT_ENUM_CAST( t3dBoundaryMode_t );
#endif // TERRAIN3D_EROSION_SETTINGS_CLASS_H
