// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_OCEAN_SETTINGS_H
#define TERRAIN3D_OCEAN_SETTINGS_H

#include "Terrain3DTypes.h"

#include <godot_cpp/classes/resource.hpp>
#include <godot_cpp/core/math_defs.hpp>
#include <godot_cpp/variant/color.hpp>

using namespace godot;

// t3dOceanSettingsState_s centralized in Terrain3DTypes.h.


class Terrain3DOceanSettings : public Resource {
	GDCLASS( Terrain3DOceanSettings, Resource );

private:
	t3dOceanSettingsState_t m_state;

public:
	Terrain3DOceanSettings( void ) = default;
	~Terrain3DOceanSettings( void ) = default;

	inline const t3dOceanSettingsState_t &getState( void ) const { return m_state; }

	void setSeaLevel( real_t fValue );
	inline real_t getSeaLevel( void ) const { return m_state.m_fSeaLevel; }
	void setWaterColor( const Color &rColor );
	inline Color getWaterColor( void ) const {
		return Color( m_state.m_szWaterColor[0], m_state.m_szWaterColor[1], m_state.m_szWaterColor[2], m_state.m_szWaterColor[3] );
	}
	void setVisibleDepth( real_t fValue );
	inline real_t getVisibleDepth( void ) const { return m_state.m_fVisibleDepth; }
	void setDepthCurve( real_t fValue );
	inline real_t getDepthCurve( void ) const { return m_state.m_fDepthCurve; }
	void setShoreBlend( real_t fValue );
	inline real_t getShoreBlend( void ) const { return m_state.m_fShoreBlend; }
	void setRefractionStrength( real_t fValue );
	inline real_t getRefractionStrength( void ) const { return m_state.m_fRefractionStrength; }
	void setTimeScale( real_t fValue );
	inline real_t getTimeScale( void ) const { return m_state.m_fTimeScale; }
	void setHeightScale( real_t fValue );
	inline real_t getHeightScale( void ) const { return m_state.m_fHeightScale; }
	void setOceanScale( real_t fValue );
	inline real_t getOceanScale( void ) const { return m_state.m_fOceanScale; }
	void setFresnelScale( real_t fValue );
	inline real_t getFresnelScale( void ) const { return m_state.m_fFresnelScale; }
	void setFresnelPower( real_t fValue );
	inline real_t getFresnelPower( void ) const { return m_state.m_fFresnelPower; }
	void setSpecular( real_t fValue );
	inline real_t getSpecular( void ) const { return m_state.m_fSpecular; }
	void setFoamHeight( real_t fValue );
	inline real_t getFoamHeight( void ) const { return m_state.m_fFoamHeight; }
	void setFoamTopIntensity( real_t fValue );
	inline real_t getFoamTopIntensity( void ) const { return m_state.m_fFoamTopIntensity; }
	void setFoamDistance( real_t fValue );
	inline real_t getFoamDistance( void ) const { return m_state.m_fFoamDistance; }
	void setFoamShoreIntensity( real_t fValue );
	inline real_t getFoamShoreIntensity( void ) const { return m_state.m_fFoamShoreIntensity; }
	void setFoamNoiseScale( real_t fValue );
	inline real_t getFoamNoiseScale( void ) const { return m_state.m_fFoamNoiseScale; }
	void setHeightScattering( real_t fValue );
	inline real_t getHeightScattering( void ) const { return m_state.m_fHeightScattering; }
	void setHeightThreshold( real_t fValue );
	inline real_t getHeightThreshold( void ) const { return m_state.m_fHeightThreshold; }
	void setHeightAngleThreshold( real_t fValue );
	inline real_t getHeightAngleThreshold( void ) const { return m_state.m_fHeightAngleThreshold; }
	void setFresnelScattering( real_t fValue );
	inline real_t getFresnelScattering( void ) const { return m_state.m_fFresnelScattering; }
	void setDirectScattering( real_t fValue );
	inline real_t getDirectScattering( void ) const { return m_state.m_fDirectScattering; }

protected:
	static void _bind_methods( void );
};

#endif // TERRAIN3D_OCEAN_SETTINGS_H
