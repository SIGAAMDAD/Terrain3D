// Editor-only foliage generation. Runtime instance stores remain authoritative.
#ifndef TERRAIN3D_FOLIAGE_GENERATOR_H
#include "Terrain3DTypes.h"
#define TERRAIN3D_FOLIAGE_GENERATOR_H

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/typed_array.hpp>
#include <godot_cpp/variant/vector2i.hpp>

#if defined( GDEXTENSION ) && !defined( GODOT_MODULE )
using namespace godot;
#endif

class Terrain3D;

class Terrain3DFoliageGenerator : public RefCounted
{
	GDCLASS( Terrain3DFoliageGenerator, RefCounted );
	CLASS_NAME();

	Terrain3D				 *m_pTerrain = nullptr;
	Array					  m_rules;
	TypedArray< Vector2i >	  m_regions;
	Array					  m_batches;
	Dictionary				  m_rule;
	TypedArray< Transform3D > m_transforms;
	int						  m_nRegion = 0;
	int						  m_nRule = 0;
	int64_t					  m_nCandidate = 0;
	int64_t					  m_nCandidateCount = 0;
	int						  m_nGridMinX = 0;
	int						  m_nGridMinZ = 0;
	int						  m_nGridWidth = 0;
	int						  m_nGridHeight = 0;
	double					  m_fCellSize = 1.0;
	uint64_t				  m_nSeed = 0;
	int						  m_nRegionCap = 50000;
	int						  m_nJobCap = 250000;
	int64_t					  m_nMaxScratchBytes = 67108864;
	int						  m_nRegionAdded = 0;
	int						  m_nAdded = 0;
	bool					  m_bRunning = false;
	bool					  m_bFailed = false;
	String					  m_error;

	bool			prepareRule( void );
	bool			sampleCandidate( void );
	bool			finishRule( void );
	static uint64_t hash64( uint64_t n );
	static double	randomUnit( uint64_t n );
	static uint64_t hashName( const String &rValue );

protected:
	static void _bind_methods( void );

public:
	bool	   start( Terrain3D *pTerrain, const Dictionary &rConfig, const TypedArray< Vector2i > &rRegions );
	Dictionary step( int nBudget = 2048 );
	void	   cancel( void );
	Array	   takeBatches( void );
	Dictionary status( void ) const;
};

#endif
