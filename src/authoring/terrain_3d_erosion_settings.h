// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#pragma once

#ifndef TERRAIN3D_EROSION_SETTINGS_CLASS_H
#define TERRAIN3D_EROSION_SETTINGS_CLASS_H

#include <godot_cpp/classes/curve.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/resource.hpp>

#include "Terrain3DConstants.h"

class Terrain3DErosionSettings : public Resource {
	GDCLASS( Terrain3DErosionSettings, Resource );
	CLASS_NAME();

public:
	typedef enum t3dboundaryMode_e {
		BOUNDARY_CLOSED,
		BOUNDARY_OPEN,
	} t3dboundaryMode_t;

private:
	// General.
	double m_fSimulationYears = 1000.0;
	int32_t m_nSimulationSteps = 128;
	int32_t m_resolutionScale = 8;
	bool m_bAutoAdjustResolution = true;
	int32_t m_nMemoryBudgetMb = 1024;
	bool m_bLivePreviewEnabled = true;
	int32_t m_nLivePreviewIntervalSteps = 4;
	bool m_bLivePreviewStartPaused = false;
	int32_t m_nThreadCount = 0;
	int64_t m_seed = 1;
	t3dboundaryMode_t m_boundaryMode = BOUNDARY_CLOSED;
	float m_fMaxHeightChangePerStep = 0.5f;
	bool m_bPreserveHoles = true;
	bool m_bApplyHeight = true;
	float m_fHeightOutputStrength = 1.0f;
	float m_fGlobalIntensity = 1.0f;
	Ref<Image> m_pSimulationMask;
	Rect2 m_simulationMaskWorldRect;
	bool m_bInvertSimulationMask = false;
	float m_fEdgeFadeDistanceM = 0.0f;
	bool m_bHeightRangeEnabled = false;
	float m_fMinimumHeightM = -1000000.0f;
	float m_fMaximumHeightM = 1000000.0f;
	float m_fHeightRangeFalloffM = 0.0f;

	// Hydraulic erosion.
	bool m_bHydraulicEnabled = true;
	float m_fRainfallMPerYear = 0.002f;
	float m_fRainfallVariation = 0.25f;
	float m_fEvaporationFraction = 0.18f;
	float m_fInfiltrationFraction = 0.05f;
	float m_fFlowRate = 0.65f;
	float m_fSedimentCapacity = 1.25f;
	float m_fHydraulicErosionMPerYear = 0.0002f;
	float m_fHydraulicDepositionRate = 0.35f;
	float m_fMinimumSlope = 0.001f;
	float m_fStormFrequency = 1.0f;
	float m_fStormIntensity = 1.0f;
	float m_fChannelIncisionStrength = 0.0f;
	float m_fStandingWaterDepositionRate = 0.0f;

	// Desert sediment sorting, fans, and debris flows.
	bool m_bSedimentSortingEnabled = true;
	float m_fCoarseTransportMultiplier = 0.35f;
	float m_fSandTransportMultiplier = 1.0f;
	float m_fFineTransportMultiplier = 1.5f;
	float m_fCoarseSettlingRate = 0.75f;
	float m_fSandSettlingRate = 0.35f;
	float m_fFineSettlingRate = 0.08f;
	bool m_bAlluvialFanEnabled = false;
	float m_fAlluvialFanStrength = 0.75f;
	float m_fAlluvialFanSlopeDrop = 0.02f;
	float m_fAlluvialFanSpread = 0.35f;
	bool m_bDebrisFlowEnabled = false;
	float m_fDebrisFlowConcentrationThreshold = 0.35f;
	float m_fDebrisFlowSlopeThreshold = 0.02f;
	float m_fDebrisFlowRate = 0.35f;
	float m_fDebrisFlowDepositionRate = 0.65f;
	int32_t m_nDebrisFlowRunoutCells = 4;

	// Weathering and oxidation.
	bool m_bWeatheringEnabled = true;
	float m_fPhysicalWeatheringMPerYear = 0.00005f;
	float m_fChemicalWeatheringMPerYear = 0.00002f;
	float m_fFreezeThawStrength = 0.25f;
	float m_fTalusAngleDegrees = 35.0f;
	float m_fThermalCreepRate = 0.2f;
	float m_fBaseHardness = 0.72f;
	float m_fHardnessVariation = 0.12f;
	bool m_bOxidationEnabled = true;
	float m_fOxidationRatePerYear = 0.0001f;
	float m_fOxidationWeakening = 0.2f;
	float m_fWeatheringSlopeBias = 1.0f;
	float m_fChemicalMoistureExponent = 1.0f;
	float m_fFreezePoint = 0.0f;

	// Arid weathering, salinity, exposure age, and seasonal melt.
	bool m_bWetDryEnabled = false;
	float m_fWetDryWeatheringMPerCycle = 0.0005f;
	float m_fWetThreshold = 0.05f;
	float m_fDryThreshold = 0.01f;
	bool m_bSaltWeatheringEnabled = false;
	Ref<Image> m_pSalinityMask;
	Rect2 m_salinityWorldRect;
	float m_fBackgroundSalinity = 0.0f;
	float m_fSaltWeatheringMPerYear = 0.00005f;
	float m_fSaltCrystallizationStrength = 1.0f;
	float m_fSaltEvaporationBias = 1.0f;
	float m_fSaltCohesionLoss = 0.25f;
	bool m_bExposureAgeEnabled = false;
	float m_fVarnishMaturationYears = 5000.0f;
	float m_fExposureResetDepthM = 0.02f;
	float m_fVarnishStabilitySlope = 0.25f;
	float m_fVarnishOxidationMultiplier = 1.0f;
	bool m_bSnowmeltEnabled = false;
	float m_fSnowfallFraction = 0.65f;
	float m_fSnowAccumulationTemperature = 0.0f;
	float m_fSnowMeltTemperature = 0.1f;
	float m_fSnowMeltFractionPerStep = 0.25f;
	float m_fSnowSublimationFraction = 0.02f;

	// Wind.
	bool m_bWindEnabled = false;
	Vector2 m_windDirection = Vector2( 1.0f, 0.0f );
	float m_fWindStrength = 1.0f;
	float m_fWindVariation = 0.2f;
	float m_fWindErosionMPerYear = 0.00005f;
	float m_fWindTransportRate = 0.25f;
	float m_fWindDepositionRate = 0.4f;
	float m_fWindShelterStrength = 0.6f;
	float m_fWindThreshold = 0.0f;
	int32_t m_nWindTransportDistanceCells = 1;
	float m_fWindDepositionSlopeBias = 0.0f;

	// Dune relaxation and desert pavement.
	bool m_bDuneEnabled = false;
	float m_fSandReposeAngleDegrees = 33.0f;
	float m_fDuneAvalancheRate = 0.35f;
	int32_t m_nDuneRelaxationIterations = 2;
	float m_fDuneMinSandDepthM = 0.02f;
	bool m_bDesertPavementEnabled = false;
	float m_fPavementFormationRate = 0.25f;
	float m_fPavementFineRemovalThreshold = 0.4f;
	float m_fPavementArmorStrength = 0.65f;
	float m_fPavementInfiltrationReduction = 0.25f;

	// Waves.
	bool m_bWaveEnabled = false;
	float m_fSeaLevel = 0.0f;
	float m_fTidalRange = 2.0f;
	Vector2 m_waveDirection = Vector2( 1.0f, 0.0f );
	float m_fWaveStrength = 1.0f;
	float m_fWaveErosionMPerYear = 0.001f;
	float m_fWaveDepositionRate = 0.65f;
	float m_fCoastlineBandM = 12.0f;
	float m_fWaveFetchDistanceM = 512.0f;
	float m_fWaveUndercutBias = 1.5f;
	float m_fBeachSmoothing = 0.0f;

	// Foot traffic.
	bool m_bTrafficEnabled = false;
	Ref<Image> m_pTrafficMask;
	Rect2 m_trafficWorldRect;
	float m_fTrafficErosionMPerYear = 0.0002f;
	float m_fTrafficCompaction = 0.75f;
	float m_fTrafficSmoothing = 0.2f;
	float m_fTrafficRunoffMultiplier = 1.5f;
	float m_fTrafficThreshold = 0.0f;
	int32_t m_nTrafficWideningCells = 0;

	// Layered substrate / stratigraphy.
	bool m_bStratigraphyEnabled = false;
	float m_fSurfaceLayerDepthM = 2.0f;
	Ref<Image> m_pSurfaceLayerDepthMap;
	Rect2 m_surfaceLayerDepthWorldRect;
	float m_fBedrockHardness = 0.9f;
	Ref<Image> m_pBedrockHardnessMap;
	Rect2 m_bedrockHardnessWorldRect;
	float m_fStratigraphyTransitionWidthM = 0.5f;

	// Material transitions.
	bool m_bMaterialOutputEnabled = false;
	bool m_bPreserveControlFlags = true;
	bool m_bResolveAutoMaterials = true;
	float m_fMaterialTransitionStrength = 1.0f;
	float m_fMaterialTransitionThreshold = 0.05f;
	float m_fMaterialTransitionDepthM = 0.25f;
	bool m_bOxidationColorEnabled = false;
	Color m_oxidationTint = Color( 0.45f, 0.19f, 0.07f, 1.0f );
	float m_fOxidationTintStrength = 0.45f;

	// Curves are sampled on the main thread before simulation starts.
	Ref<Curve> m_pMasterIntensityCurve;
	Ref<Curve> m_pRainfallCurve;
	Ref<Curve> m_pWeatheringCurve;
	Ref<Curve> m_pOxidationCurve;
	Ref<Curve> m_pWindCurve;
	Ref<Curve> m_pWaveCurve;
	Ref<Curve> m_pTrafficCurve;
	Ref<Curve> m_pTemperatureCurve;
	Ref<Curve> m_pSeaLevelCurve;
	Ref<Curve> m_pSnowmeltCurve;
	Ref<Curve> m_pSaltWeatheringCurve;
	Ref<Curve> m_pDebrisFlowCurve;

	static Ref<Curve> makeFlatCurve( float fValue, float fMin = 0.0f, float fMax = 2.0f );
	void setCurve( Ref<Curve> &rTargetCurve, const Ref<Curve> &rSourceCurve );

public:
	Terrain3DErosionSettings( void );
	~Terrain3DErosionSettings( void ) = default;

	void setSimulationYears( double fValue );
	double getSimulationYears( void ) const {
		return m_fSimulationYears;
	}
	void setSimulationSteps( int32_t nValue );
	int32_t getSimulationSteps( void ) const {
		return m_nSimulationSteps;
	}
	void setResolutionScale( int32_t nValue );
	int32_t getResolutionScale( void ) const {
		return m_resolutionScale;
	}
	void setAutoAdjustResolution( bool bValue );
	bool getAutoAdjustResolution( void ) const {
		return m_bAutoAdjustResolution;
	}
	void setMemoryBudgetMb( int32_t nValue );
	int32_t getMemoryBudgetMb( void ) const {
		return m_nMemoryBudgetMb;
	}
	void setLivePreviewEnabled( bool bValue );
	bool getLivePreviewEnabled( void ) const {
		return m_bLivePreviewEnabled;
	}
	void setLivePreviewIntervalSteps( int32_t nValue );
	int32_t getLivePreviewIntervalSteps( void ) const {
		return m_nLivePreviewIntervalSteps;
	}
	void setLivePreviewStartPaused( bool bValue );
	bool getLivePreviewStartPaused( void ) const {
		return m_bLivePreviewStartPaused;
	}
	void setThreadCount( int32_t nValue );
	int32_t getThreadCount( void ) const {
		return m_nThreadCount;
	}
	void setSeed( int64_t nValue );
	int64_t getSeed( void ) const {
		return m_seed;
	}
	void setBoundaryMode( t3dboundaryMode_t eValue );
	t3dboundaryMode_t getBoundaryMode( void ) const {
		return m_boundaryMode;
	}
	void setMaxHeightChangePerStep( float fValue );
	float getMaxHeightChangePerStep( void ) const {
		return m_fMaxHeightChangePerStep;
	}
	void setPreserveHoles( bool bValue );
	bool getPreserveHoles( void ) const {
		return m_bPreserveHoles;
	}
	void setApplyHeight( bool bValue );
	bool getApplyHeight( void ) const {
		return m_bApplyHeight;
	}
	void setHeightOutputStrength( float fValue );
	float getHeightOutputStrength( void ) const {
		return m_fHeightOutputStrength;
	}
	void setGlobalIntensity( float fValue );
	float getGlobalIntensity( void ) const {
		return m_fGlobalIntensity;
	}
	void setSimulationMask( const Ref<Image> &rValue );
	Ref<Image> getSimulationMask( void ) const {
		return m_pSimulationMask;
	}
	void setSimulationMaskWorldRect( const Rect2 &rValue );
	Rect2 getSimulationMaskWorldRect( void ) const {
		return m_simulationMaskWorldRect;
	}
	void setInvertSimulationMask( bool bValue );
	bool getInvertSimulationMask( void ) const {
		return m_bInvertSimulationMask;
	}
	void setEdgeFadeDistanceM( float fValue );
	float getEdgeFadeDistanceM( void ) const {
		return m_fEdgeFadeDistanceM;
	}
	void setHeightRangeEnabled( bool bValue );
	bool getHeightRangeEnabled( void ) const {
		return m_bHeightRangeEnabled;
	}
	void setMinimumHeightM( float fValue );
	float getMinimumHeightM( void ) const {
		return m_fMinimumHeightM;
	}
	void setMaximumHeightM( float fValue );
	float getMaximumHeightM( void ) const {
		return m_fMaximumHeightM;
	}
	void setHeightRangeFalloffM( float fValue );
	float getHeightRangeFalloffM( void ) const {
		return m_fHeightRangeFalloffM;
	}

	void setHydraulicEnabled( bool bValue );
	bool getHydraulicEnabled( void ) const {
		return m_bHydraulicEnabled;
	}
	void setRainfallMPerYear( float fValue );
	float getRainfallMPerYear( void ) const {
		return m_fRainfallMPerYear;
	}
	void setRainfallVariation( float fValue );
	float getRainfallVariation( void ) const {
		return m_fRainfallVariation;
	}
	void setEvaporationFraction( float fValue );
	float getEvaporationFraction( void ) const {
		return m_fEvaporationFraction;
	}
	void setInfiltrationFraction( float fValue );
	float getInfiltrationFraction( void ) const {
		return m_fInfiltrationFraction;
	}
	void setFlowRate( float fValue );
	float getFlowRate( void ) const {
		return m_fFlowRate;
	}
	void setSedimentCapacity( float fValue );
	float getSedimentCapacity( void ) const {
		return m_fSedimentCapacity;
	}
	void setHydraulicErosionMPerYear( float fValue );
	float getHydraulicErosionMPerYear( void ) const {
		return m_fHydraulicErosionMPerYear;
	}
	void setHydraulicDepositionRate( float fValue );
	float getHydraulicDepositionRate( void ) const {
		return m_fHydraulicDepositionRate;
	}
	void setMinimumSlope( float fValue );
	float getMinimumSlope( void ) const {
		return m_fMinimumSlope;
	}
	void setStormFrequency( float fValue );
	float getStormFrequency( void ) const {
		return m_fStormFrequency;
	}
	void setStormIntensity( float fValue );
	float getStormIntensity( void ) const {
		return m_fStormIntensity;
	}
	void setChannelIncisionStrength( float fValue );
	float getChannelIncisionStrength( void ) const {
		return m_fChannelIncisionStrength;
	}
	void setStandingWaterDepositionRate( float fValue );
	float getStandingWaterDepositionRate( void ) const {
		return m_fStandingWaterDepositionRate;
	}
	void setSedimentSortingEnabled( bool bValue );
	bool getSedimentSortingEnabled( void ) const {
		return m_bSedimentSortingEnabled;
	}
	void setCoarseTransportMultiplier( float fValue );
	float getCoarseTransportMultiplier( void ) const {
		return m_fCoarseTransportMultiplier;
	}
	void setSandTransportMultiplier( float fValue );
	float getSandTransportMultiplier( void ) const {
		return m_fSandTransportMultiplier;
	}
	void setFineTransportMultiplier( float fValue );
	float getFineTransportMultiplier( void ) const {
		return m_fFineTransportMultiplier;
	}
	void setCoarseSettlingRate( float fValue );
	float getCoarseSettlingRate( void ) const {
		return m_fCoarseSettlingRate;
	}
	void setSandSettlingRate( float fValue );
	float getSandSettlingRate( void ) const {
		return m_fSandSettlingRate;
	}
	void setFineSettlingRate( float fValue );
	float getFineSettlingRate( void ) const {
		return m_fFineSettlingRate;
	}
	void setAlluvialFanEnabled( bool bValue );
	bool getAlluvialFanEnabled( void ) const {
		return m_bAlluvialFanEnabled;
	}
	void setAlluvialFanStrength( float fValue );
	float getAlluvialFanStrength( void ) const {
		return m_fAlluvialFanStrength;
	}
	void setAlluvialFanSlopeDrop( float fValue );
	float getAlluvialFanSlopeDrop( void ) const {
		return m_fAlluvialFanSlopeDrop;
	}
	void setAlluvialFanSpread( float fValue );
	float getAlluvialFanSpread( void ) const {
		return m_fAlluvialFanSpread;
	}
	void setDebrisFlowEnabled( bool bValue );
	bool getDebrisFlowEnabled( void ) const {
		return m_bDebrisFlowEnabled;
	}
	void setDebrisFlowConcentrationThreshold( float fValue );
	float getDebrisFlowConcentrationThreshold( void ) const {
		return m_fDebrisFlowConcentrationThreshold;
	}
	void setDebrisFlowSlopeThreshold( float fValue );
	float getDebrisFlowSlopeThreshold( void ) const {
		return m_fDebrisFlowSlopeThreshold;
	}
	void setDebrisFlowRate( float fValue );
	float getDebrisFlowRate( void ) const {
		return m_fDebrisFlowRate;
	}
	void setDebrisFlowDepositionRate( float fValue );
	float getDebrisFlowDepositionRate( void ) const {
		return m_fDebrisFlowDepositionRate;
	}
	void setDebrisFlowRunoutCells( int32_t nValue );
	int32_t getDebrisFlowRunoutCells( void ) const {
		return m_nDebrisFlowRunoutCells;
	}

	void setWeatheringEnabled( bool bValue );
	bool getWeatheringEnabled( void ) const {
		return m_bWeatheringEnabled;
	}
	void setPhysicalWeatheringMPerYear( float fValue );
	float getPhysicalWeatheringMPerYear( void ) const {
		return m_fPhysicalWeatheringMPerYear;
	}
	void setChemicalWeatheringMPerYear( float fValue );
	float getChemicalWeatheringMPerYear( void ) const {
		return m_fChemicalWeatheringMPerYear;
	}
	void setFreezeThawStrength( float fValue );
	float getFreezeThawStrength( void ) const {
		return m_fFreezeThawStrength;
	}
	void setTalusAngleDegrees( float fValue );
	float getTalusAngleDegrees( void ) const {
		return m_fTalusAngleDegrees;
	}
	void setThermalCreepRate( float fValue );
	float getThermalCreepRate( void ) const {
		return m_fThermalCreepRate;
	}
	void setBaseHardness( float fValue );
	float getBaseHardness( void ) const {
		return m_fBaseHardness;
	}
	void setHardnessVariation( float fValue );
	float getHardnessVariation( void ) const {
		return m_fHardnessVariation;
	}
	void setOxidationEnabled( bool bValue );
	bool getOxidationEnabled( void ) const {
		return m_bOxidationEnabled;
	}
	void setOxidationRatePerYear( float fValue );
	float getOxidationRatePerYear( void ) const {
		return m_fOxidationRatePerYear;
	}
	void setOxidationWeakening( float fValue );
	float getOxidationWeakening( void ) const {
		return m_fOxidationWeakening;
	}
	void setWeatheringSlopeBias( float fValue );
	float getWeatheringSlopeBias( void ) const {
		return m_fWeatheringSlopeBias;
	}
	void setChemicalMoistureExponent( float fValue );
	float getChemicalMoistureExponent( void ) const {
		return m_fChemicalMoistureExponent;
	}
	void setFreezePoint( float fValue );
	float getFreezePoint( void ) const {
		return m_fFreezePoint;
	}
	void setWetDryEnabled( bool bValue );
	bool getWetDryEnabled( void ) const {
		return m_bWetDryEnabled;
	}
	void setWetDryWeatheringMPerCycle( float fValue );
	float getWetDryWeatheringMPerCycle( void ) const {
		return m_fWetDryWeatheringMPerCycle;
	}
	void setWetThreshold( float fValue );
	float getWetThreshold( void ) const {
		return m_fWetThreshold;
	}
	void setDryThreshold( float fValue );
	float getDryThreshold( void ) const {
		return m_fDryThreshold;
	}
	void setSaltWeatheringEnabled( bool bValue );
	bool getSaltWeatheringEnabled( void ) const {
		return m_bSaltWeatheringEnabled;
	}
	void setSalinityMask( const Ref<Image> &rValue );
	Ref<Image> getSalinityMask( void ) const {
		return m_pSalinityMask;
	}
	void setSalinityWorldRect( const Rect2 &rValue );
	Rect2 getSalinityWorldRect( void ) const {
		return m_salinityWorldRect;
	}
	void setBackgroundSalinity( float fValue );
	float getBackgroundSalinity( void ) const {
		return m_fBackgroundSalinity;
	}
	void setSaltWeatheringMPerYear( float fValue );
	float getSaltWeatheringMPerYear( void ) const {
		return m_fSaltWeatheringMPerYear;
	}
	void setSaltCrystallizationStrength( float fValue );
	float getSaltCrystallizationStrength( void ) const {
		return m_fSaltCrystallizationStrength;
	}
	void setSaltEvaporationBias( float fValue );
	float getSaltEvaporationBias( void ) const {
		return m_fSaltEvaporationBias;
	}
	void setSaltCohesionLoss( float fValue );
	float getSaltCohesionLoss( void ) const {
		return m_fSaltCohesionLoss;
	}
	void setExposureAgeEnabled( bool bValue );
	bool getExposureAgeEnabled( void ) const {
		return m_bExposureAgeEnabled;
	}
	void setVarnishMaturationYears( float fValue );
	float getVarnishMaturationYears( void ) const {
		return m_fVarnishMaturationYears;
	}
	void setExposureResetDepthM( float fValue );
	float getExposureResetDepthM( void ) const {
		return m_fExposureResetDepthM;
	}
	void setVarnishStabilitySlope( float fValue );
	float getVarnishStabilitySlope( void ) const {
		return m_fVarnishStabilitySlope;
	}
	void setVarnishOxidationMultiplier( float fValue );
	float getVarnishOxidationMultiplier( void ) const {
		return m_fVarnishOxidationMultiplier;
	}
	void setSnowmeltEnabled( bool bValue );
	bool getSnowmeltEnabled( void ) const {
		return m_bSnowmeltEnabled;
	}
	void setSnowfallFraction( float fValue );
	float getSnowfallFraction( void ) const {
		return m_fSnowfallFraction;
	}
	void setSnowAccumulationTemperature( float fValue );
	float getSnowAccumulationTemperature( void ) const {
		return m_fSnowAccumulationTemperature;
	}
	void setSnowMeltTemperature( float fValue );
	float getSnowMeltTemperature( void ) const {
		return m_fSnowMeltTemperature;
	}
	void setSnowMeltFractionPerStep( float fValue );
	float getSnowMeltFractionPerStep( void ) const {
		return m_fSnowMeltFractionPerStep;
	}
	void setSnowSublimationFraction( float fValue );
	float getSnowSublimationFraction( void ) const {
		return m_fSnowSublimationFraction;
	}

	void setWindEnabled( bool bValue );
	bool getWindEnabled( void ) const {
		return m_bWindEnabled;
	}
	void setWindDirection( const Vector2 &vValue );
	Vector2 getWindDirection( void ) const {
		return m_windDirection;
	}
	void setWindStrength( float fValue );
	float getWindStrength( void ) const {
		return m_fWindStrength;
	}
	void setWindVariation( float fValue );
	float getWindVariation( void ) const {
		return m_fWindVariation;
	}
	void setWindErosionMPerYear( float fValue );
	float getWindErosionMPerYear( void ) const {
		return m_fWindErosionMPerYear;
	}
	void setWindTransportRate( float fValue );
	float getWindTransportRate( void ) const {
		return m_fWindTransportRate;
	}
	void setWindDepositionRate( float fValue );
	float getWindDepositionRate( void ) const {
		return m_fWindDepositionRate;
	}
	void setWindShelterStrength( float fValue );
	float getWindShelterStrength( void ) const {
		return m_fWindShelterStrength;
	}
	void setWindThreshold( float fValue );
	float getWindThreshold( void ) const {
		return m_fWindThreshold;
	}
	void setWindTransportDistanceCells( int32_t nValue );
	int32_t getWindTransportDistanceCells( void ) const {
		return m_nWindTransportDistanceCells;
	}
	void setWindDepositionSlopeBias( float fValue );
	float getWindDepositionSlopeBias( void ) const {
		return m_fWindDepositionSlopeBias;
	}
	void setDuneEnabled( bool bValue );
	bool getDuneEnabled( void ) const {
		return m_bDuneEnabled;
	}
	void setSandReposeAngleDegrees( float fValue );
	float getSandReposeAngleDegrees( void ) const {
		return m_fSandReposeAngleDegrees;
	}
	void setDuneAvalancheRate( float fValue );
	float getDuneAvalancheRate( void ) const {
		return m_fDuneAvalancheRate;
	}
	void setDuneRelaxationIterations( int32_t nValue );
	int32_t getDuneRelaxationIterations( void ) const {
		return m_nDuneRelaxationIterations;
	}
	void setDuneMinSandDepthM( float fValue );
	float getDuneMinSandDepthM( void ) const {
		return m_fDuneMinSandDepthM;
	}
	void setDesertPavementEnabled( bool bValue );
	bool getDesertPavementEnabled( void ) const {
		return m_bDesertPavementEnabled;
	}
	void setPavementFormationRate( float fValue );
	float getPavementFormationRate( void ) const {
		return m_fPavementFormationRate;
	}
	void setPavementFineRemovalThreshold( float fValue );
	float getPavementFineRemovalThreshold( void ) const {
		return m_fPavementFineRemovalThreshold;
	}
	void setPavementArmorStrength( float fValue );
	float getPavementArmorStrength( void ) const {
		return m_fPavementArmorStrength;
	}
	void setPavementInfiltrationReduction( float fValue );
	float getPavementInfiltrationReduction( void ) const {
		return m_fPavementInfiltrationReduction;
	}

	void setWaveEnabled( bool bValue );
	bool getWaveEnabled( void ) const {
		return m_bWaveEnabled;
	}
	void setSeaLevel( float fValue );
	float getSeaLevel( void ) const {
		return m_fSeaLevel;
	}
	void setTidalRange( float fValue );
	float getTidalRange( void ) const {
		return m_fTidalRange;
	}
	void setWaveDirection( const Vector2 &vValue );
	Vector2 getWaveDirection( void ) const {
		return m_waveDirection;
	}
	void setWaveStrength( float fValue );
	float getWaveStrength( void ) const {
		return m_fWaveStrength;
	}
	void setWaveErosionMPerYear( float fValue );
	float getWaveErosionMPerYear( void ) const {
		return m_fWaveErosionMPerYear;
	}
	void setWaveDepositionRate( float fValue );
	float getWaveDepositionRate( void ) const {
		return m_fWaveDepositionRate;
	}
	void setCoastlineBandM( float fValue );
	float getCoastlineBandM( void ) const {
		return m_fCoastlineBandM;
	}
	void setWaveFetchDistanceM( float fValue );
	float getWaveFetchDistanceM( void ) const {
		return m_fWaveFetchDistanceM;
	}
	void setWaveUndercutBias( float fValue );
	float getWaveUndercutBias( void ) const {
		return m_fWaveUndercutBias;
	}
	void setBeachSmoothing( float fValue );
	float getBeachSmoothing( void ) const {
		return m_fBeachSmoothing;
	}

	void setTrafficEnabled( bool bValue );
	bool getTrafficEnabled( void ) const {
		return m_bTrafficEnabled;
	}
	void setTrafficMask( const Ref<Image> &rValue );
	Ref<Image> getTrafficMask( void ) const {
		return m_pTrafficMask;
	}
	void setTrafficWorldRect( const Rect2 &rValue );
	Rect2 getTrafficWorldRect( void ) const {
		return m_trafficWorldRect;
	}
	void setTrafficErosionMPerYear( float fValue );
	float getTrafficErosionMPerYear( void ) const {
		return m_fTrafficErosionMPerYear;
	}
	void setTrafficCompaction( float fValue );
	float getTrafficCompaction( void ) const {
		return m_fTrafficCompaction;
	}
	void setTrafficSmoothing( float fValue );
	float getTrafficSmoothing( void ) const {
		return m_fTrafficSmoothing;
	}
	void setTrafficRunoffMultiplier( float fValue );
	float getTrafficRunoffMultiplier( void ) const {
		return m_fTrafficRunoffMultiplier;
	}
	void setTrafficThreshold( float fValue );
	float getTrafficThreshold( void ) const {
		return m_fTrafficThreshold;
	}
	void setTrafficWideningCells( int32_t nValue );
	int32_t getTrafficWideningCells( void ) const {
		return m_nTrafficWideningCells;
	}

	void setStratigraphyEnabled( bool bValue );
	bool getStratigraphyEnabled( void ) const {
		return m_bStratigraphyEnabled;
	}
	void setSurfaceLayerDepthM( float fValue );
	float getSurfaceLayerDepthM( void ) const {
		return m_fSurfaceLayerDepthM;
	}
	void setSurfaceLayerDepthMap( const Ref<Image> &rValue );
	Ref<Image> getSurfaceLayerDepthMap( void ) const {
		return m_pSurfaceLayerDepthMap;
	}
	void setSurfaceLayerDepthWorldRect( const Rect2 &rValue );
	Rect2 getSurfaceLayerDepthWorldRect( void ) const {
		return m_surfaceLayerDepthWorldRect;
	}
	void setBedrockHardness( float fValue );
	float getBedrockHardness( void ) const {
		return m_fBedrockHardness;
	}
	void setBedrockHardnessMap( const Ref<Image> &rValue );
	Ref<Image> getBedrockHardnessMap( void ) const {
		return m_pBedrockHardnessMap;
	}
	void setBedrockHardnessWorldRect( const Rect2 &rValue );
	Rect2 getBedrockHardnessWorldRect( void ) const {
		return m_bedrockHardnessWorldRect;
	}
	void setStratigraphyTransitionWidthM( float fValue );
	float getStratigraphyTransitionWidthM( void ) const {
		return m_fStratigraphyTransitionWidthM;
	}

	void setMaterialOutputEnabled( bool bValue );
	bool getMaterialOutputEnabled( void ) const {
		return m_bMaterialOutputEnabled;
	}
	void setPreserveControlFlags( bool bValue );
	bool getPreserveControlFlags( void ) const {
		return m_bPreserveControlFlags;
	}
	void setResolveAutoMaterials( bool bValue );
	bool getResolveAutoMaterials( void ) const {
		return m_bResolveAutoMaterials;
	}
	void setMaterialTransitionStrength( float fValue );
	float getMaterialTransitionStrength( void ) const {
		return m_fMaterialTransitionStrength;
	}
	void setMaterialTransitionThreshold( float fValue );
	float getMaterialTransitionThreshold( void ) const {
		return m_fMaterialTransitionThreshold;
	}
	void setMaterialTransitionDepthM( float fValue );
	float getMaterialTransitionDepthM( void ) const {
		return m_fMaterialTransitionDepthM;
	}
	void setOxidationColorEnabled( bool bValue );
	bool getOxidationColorEnabled( void ) const {
		return m_bOxidationColorEnabled;
	}
	void setOxidationTint( const Color &clrValue );
	Color getOxidationTint( void ) const {
		return m_oxidationTint;
	}
	void setOxidationTintStrength( float fValue );
	float getOxidationTintStrength( void ) const {
		return m_fOxidationTintStrength;
	}

	void setMasterIntensityCurve( const Ref<Curve> &rValue );
	Ref<Curve> getMasterIntensityCurve( void ) const {
		return m_pMasterIntensityCurve;
	}
	void setRainfallCurve( const Ref<Curve> &rValue );
	Ref<Curve> getRainfallCurve( void ) const {
		return m_pRainfallCurve;
	}
	void setWeatheringCurve( const Ref<Curve> &rValue );
	Ref<Curve> getWeatheringCurve( void ) const {
		return m_pWeatheringCurve;
	}
	void setOxidationCurve( const Ref<Curve> &rValue );
	Ref<Curve> getOxidationCurve( void ) const {
		return m_pOxidationCurve;
	}
	void setWindCurve( const Ref<Curve> &rValue );
	Ref<Curve> getWindCurve( void ) const {
		return m_pWindCurve;
	}
	void setWaveCurve( const Ref<Curve> &rValue );
	Ref<Curve> getWaveCurve( void ) const {
		return m_pWaveCurve;
	}
	void setTrafficCurve( const Ref<Curve> &rValue );
	Ref<Curve> getTrafficCurve( void ) const {
		return m_pTrafficCurve;
	}
	void setTemperatureCurve( const Ref<Curve> &rValue );
	Ref<Curve> getTemperatureCurve( void ) const {
		return m_pTemperatureCurve;
	}
	void setSeaLevelCurve( const Ref<Curve> &rValue );
	Ref<Curve> getSeaLevelCurve( void ) const {
		return m_pSeaLevelCurve;
	}
	void setSnowmeltCurve( const Ref<Curve> &rValue );
	Ref<Curve> getSnowmeltCurve( void ) const {
		return m_pSnowmeltCurve;
	}
	void setSaltWeatheringCurve( const Ref<Curve> &rValue );
	Ref<Curve> getSaltWeatheringCurve( void ) const {
		return m_pSaltWeatheringCurve;
	}
	void setDebrisFlowCurve( const Ref<Curve> &rValue );
	Ref<Curve> getDebrisFlowCurve( void ) const {
		return m_pDebrisFlowCurve;
	}

protected:
	void _validate_property( PropertyInfo &rProperty ) const;
	static void _bind_methods( void );
};

VARIANT_ENUM_CAST( Terrain3DErosionSettings::t3dboundaryMode_t );
#endif // TERRAIN3D_EROSION_SETTINGS_CLASS_H
