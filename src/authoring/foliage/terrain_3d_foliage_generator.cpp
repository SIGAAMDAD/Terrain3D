#include "authoring/foliage/terrain_3d_foliage_generator.h"
#include "terrain_3d.h"
#include "data/model/terrain_3d_data.h"
#include "assets/catalog/terrain_3d_assets.h"
#include "core/utility/terrain_3d_util.h"
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/core/math.hpp>
#include <math.h>

uint64_t Terrain3DFoliageGenerator::hash64(
	uint64_t n ) {
	n ^= n >> 30;
	n *= UINT64_C( 0xbf58476d1ce4e5b9 );
	n ^= n >> 27;
	n *= UINT64_C( 0x94d049bb133111eb );
	return n ^ ( n >> 31 );
}
double Terrain3DFoliageGenerator::randomUnit(
	uint64_t n ) {
	return ( double ) ( hash64( n ) >> 11 ) * ( 1.0 / 9007199254740992.0 );
}
uint64_t Terrain3DFoliageGenerator::hashName(
	const String &rValue ) {
	const CharString utf8 = rValue.utf8();
	uint64_t		 result = UINT64_C( 14695981039346656037 );
	for ( int i = 0; i < utf8.length(); ++i ) {
		result = ( result ^ ( uint8_t ) utf8[i] ) * UINT64_C( 1099511628211 );
	}
	return result;
}

bool Terrain3DFoliageGenerator::start(
	Terrain3D					 *pTerrain,
	const Dictionary			 &rConfig,
	const TypedArray< Vector2i > &rRegions ) {
	cancel();
	m_bFailed = false;
	m_error = "";
	if ( !pTerrain || !pTerrain->dataObject() || !pTerrain->getAssets().is_valid() || rRegions.is_empty() ) {
		m_error = "No terrain, regions or assets";
		return false;
	}
	m_pTerrain = pTerrain;
	m_regions = rRegions;
	m_rules = rConfig.get( "rules", Array() );
	m_nSeed = ( uint64_t ) ( ( int64_t ) rConfig.get( "seed", ( int64_t ) 0 ) );
	m_nRegionCap = CLAMP( ( int ) rConfig.get( "region_limit", 50000 ), 1, 1000000 );
	m_nJobCap = CLAMP( ( int ) rConfig.get( "job_limit", 250000 ), 1, 2000000 );
	m_nMaxScratchBytes = ( int64_t ) CLAMP( ( int ) rConfig.get( "scratch_mb", 64 ), 8, 512 ) * 1048576;
	m_nRegion = 0;
	m_nRule = 0;
	m_nCandidate = 0;
	m_nAdded = 0;
	m_nRegionAdded = 0;
	m_batches.clear();
	m_transforms.clear();
	if ( m_rules.is_empty() ) {
		m_error = "Preset has no enabled mesh rules";
		return false;
	}
	m_bRunning = true;
	return true;
}

bool Terrain3DFoliageGenerator::prepareRule(
	void ) {
	if ( m_nRegion >= m_regions.size() || m_nRule >= m_rules.size() ) {
		return false;
	}
	const Vector2i region = m_regions[m_nRegion];
	if ( !m_pTerrain->dataObject()->hasRegion( region ) ) {
		m_error = "Region no longer exists";
		return false;
	}
	m_rule = m_rules[m_nRule];
	// Every compiled rule belongs to exactly one requested region.
	if ( Vector2i( m_rule.get( "region", Vector2i( INT32_MAX, INT32_MAX ) ) ) != region ) {
		m_nCandidateCount = 0;
		m_nGridWidth = 0;
		return true;
	}
	const double density = ( double ) m_rule.get( "density", 0.0 );
	const double weight = ( double ) m_rule.get( "weight", 1.0 );
	if ( density <= 0.0 || weight <= 0.0 ) {
		m_nCandidateCount = 0;
		m_nGridWidth = 0;
		return true;
	}
	const int	  mesh_id = ( int ) m_rule.get( "mesh_id", -1 );
	const int64_t owner = m_rule.get( "owner", ( int64_t ) 0 );
	if ( owner <= 0 || owner > INT32_MAX || mesh_id < 0 || mesh_id >= m_pTerrain->getAssets()->getMeshCount() ) {
		m_error = "Unregistered foliage mesh or invalid owner";
		return false;
	}
	// Grid coordinates derive only from world position and rule density, never region-local position.
	m_fCellSize = 1.0 / sqrt( density * weight );
	m_fCellSize = CLAMP( m_fCellSize, 0.05, 128.0 );
	const double origin_x = ( double ) region.x * ( double ) T3D_REGION_WORLD_SIZE_METERS;
	const double origin_z = ( double ) region.y * ( double ) T3D_REGION_WORLD_SIZE_METERS;
	m_nGridMinX = ( int ) floor( origin_x / m_fCellSize );
	m_nGridMinZ = ( int ) floor( origin_z / m_fCellSize );
	const int max_x = ( int ) ceil( ( origin_x + T3D_REGION_WORLD_SIZE_METERS ) / m_fCellSize );
	const int max_z = ( int ) ceil( ( origin_z + T3D_REGION_WORLD_SIZE_METERS ) / m_fCellSize );
	m_nGridWidth = max_x - m_nGridMinX;
	m_nGridHeight = max_z - m_nGridMinZ;
	m_nCandidateCount = ( int64_t ) m_nGridWidth * ( int64_t ) m_nGridHeight;
	if ( m_nCandidateCount > 2000000 ) {
		m_error = "Rule exceeds candidate budget: reduce density";
		return false;
	}
	m_nCandidate = 0;
	m_transforms.clear();
	return true;
}

bool Terrain3DFoliageGenerator::sampleCandidate(
	void ) {
	const Vector2i region = m_regions[m_nRegion];
	const int	   gx = m_nGridMinX + ( int ) ( m_nCandidate % m_nGridWidth );
	const int	   gz = m_nGridMinZ + ( int ) ( m_nCandidate / m_nGridWidth );
	const double   density = ( double ) m_rule.get( "density", 0.0 ) * ( double ) m_rule.get( "weight", 1.0 );
	const String   uid = m_rule.get( "uid", "" );
	const uint64_t salt = hashName( uid ) ^ m_nSeed;
	const uint64_t key = hash64( ( uint64_t ) ( ( int64_t ) gx ) * UINT64_C( 0x9e3779b185ebca87 ) ^
								 ( uint64_t ) ( ( int64_t ) gz ) * UINT64_C( 0xc2b2ae3d27d4eb4f ) ^ salt );
	const double   x = ( ( double ) gx + randomUnit( key ) ) * m_fCellSize;
	const double   z = ( ( double ) gz + randomUnit( key ^ UINT64_C( 0x632be59bd9b4e019 ) ) ) * m_fCellSize;
	// When the minimum grid density is clamped, thin candidates to preserve the requested intensity.
	if ( randomUnit( key ^ UINT64_C( 0x7cd919a ) ) > MIN( 1.0, density * m_fCellSize * m_fCellSize ) ) {
		return true;
	}
	const double left = ( double ) region.x * T3D_REGION_WORLD_SIZE_METERS;
	const double top = ( double ) region.y * T3D_REGION_WORLD_SIZE_METERS;
	if ( x < left || z < top || x >= left + T3D_REGION_WORLD_SIZE_METERS || z >= top + T3D_REGION_WORLD_SIZE_METERS ) {
		return true;
	}
	Terrain3DData *const pData = m_pTerrain->dataObject();
	const Vector3		 location( ( real_t ) x, 0.0f, ( real_t ) z );
	const uint32_t		 control = pData->getControl( location );
	if ( control == UINT32_MAX || t3dIsHole( control ) ) {
		return true;
	}
	const Vector3 material = pData->getTextureId( location );
	if ( !material.is_finite() ) {
		return true;
	}
	const int target_material = m_rule.get( "material_id", -1 );
	double	  coverage = 0.0;
	if ( ( int ) material.x == target_material ) {
		coverage += 1.0 - CLAMP( ( double ) material.z, 0.0, 1.0 );
	}
	if ( ( int ) material.y == target_material ) {
		coverage += CLAMP( ( double ) material.z, 0.0, 1.0 );
	}
	const PackedInt32Array exclusions = m_rule.get( "excluded_materials", PackedInt32Array() );
	for ( int i = 0; i < exclusions.size(); ++i ) {
		const int excluded = exclusions[i];
		if ( ( ( int ) material.x == excluded && 1.0 - material.z > 0.01 ) ||
			 ( ( int ) material.y == excluded && material.z > 0.01 ) ) {
			return true;
		}
	}
	if ( randomUnit( key ^ UINT64_C( 0xaf17e22a ) ) >= coverage ) {
		return true;
	}
	const real_t height = pData->getHeight( location );
	if ( !Math::is_finite( height ) || height < ( double ) m_rule.get( "min_height", -100000.0 ) ||
		 height > ( double ) m_rule.get( "max_height", 100000.0 ) ) {
		return true;
	}
	Vector3 normal = pData->getNormal( location );
	if ( !normal.is_finite() || normal.length_squared() < 0.1 ) {
		return true;
	}
	normal.normalize();
	const double slope = Math::rad_to_deg( Math::acos( CLAMP( normal.y, ( real_t ) -1.0, ( real_t ) 1.0 ) ) );
	if ( slope < ( double ) m_rule.get( "min_slope", 0.0 ) || slope > ( double ) m_rule.get( "max_slope", 90.0 ) ) {
		return true;
	}
	// Smooth world-space cluster noise; same field is evaluated across every region border.
	const double cluster = CLAMP( ( double ) m_rule.get( "cluster", 0.0 ), 0.0, 1.0 );
	if ( cluster > 0.0 ) {
		const double   scale = MAX( 0.25, ( double ) m_rule.get( "cluster_scale", 12.0 ) );
		const int	   cx = ( int ) floor( x / scale );
		const int	   cz = ( int ) floor( z / scale );
		const double   fx = x / scale - cx;
		const double   fz = z / scale - cz;
		const double   sx = fx * fx * ( 3.0 - 2.0 * fx );
		const double   sz = fz * fz * ( 3.0 - 2.0 * fz );
		const uint64_t cluster_seed = salt ^ UINT64_C( 0x1842fabbff );
		const auto	   sample = [&]( int dx, int dz ) -> double {
			return randomUnit( cluster_seed ^
							   hash64( ( uint64_t ) ( ( int64_t ) ( cx + dx ) ) * UINT64_C( 0x9e3779b185ebca87 ) ^
									   ( uint64_t ) ( ( int64_t ) ( cz + dz ) ) * UINT64_C( 0xc2b2ae3d27d4eb4f ) ) );
		};
		const double a = sample( 0, 0 ) * ( 1 - sx ) + sample( 1, 0 ) * sx;
		const double b = sample( 0, 1 ) * ( 1 - sx ) + sample( 1, 1 ) * sx;
		const double influence = a * ( 1 - sz ) + b * sz;
		if ( randomUnit( key ^ UINT64_C( 0x80e6d4 ) ) > ( 1 - cluster ) + cluster * influence ) {
			return true;
		}
	}
	const double spacing = MAX( 0.0, ( double ) m_rule.get( "spacing", 0.0 ) );
	if ( spacing > 0.0 ) {
		const int radius = ( int ) ceil( spacing / m_fCellSize ) + 1;
		if ( radius > 32 ) {
			m_error = "Spacing halo exceeds 32 cells";
			return false;
		}
		const double rank = randomUnit( key ^ UINT64_C( 0xb2ae19 ) );
		for ( int dz = -radius; dz <= radius; ++dz ) {
			for ( int dx = -radius; dx <= radius; ++dx ) {
				if ( dx == 0 && dz == 0 ) {
					continue;
				}
				const uint64_t other =
					hash64( ( uint64_t ) ( ( int64_t ) ( gx + dx ) ) * UINT64_C( 0x9e3779b185ebca87 ) ^
							( uint64_t ) ( ( int64_t ) ( gz + dz ) ) * UINT64_C( 0xc2b2ae3d27d4eb4f ) ^ salt );
				const double ox = ( gx + dx + randomUnit( other ) ) * m_fCellSize;
				const double oz = ( gz + dz + randomUnit( other ^ UINT64_C( 0x632be59bd9b4e019 ) ) ) * m_fCellSize;
				if ( ( ox - x ) * ( ox - x ) + ( oz - z ) * ( oz - z ) >= spacing * spacing ) {
					continue;
				}
				const double other_rank = randomUnit( other ^ UINT64_C( 0xb2ae19 ) );
				if ( other_rank < rank || ( other_rank == rank && ( dz < 0 || ( dz == 0 && dx < 0 ) ) ) ) {
					return true;
				}
			}
		}
	}
	const double min_scale = MAX( 0.01, ( double ) m_rule.get( "min_scale", 1.0 ) );
	const double max_scale = MAX( min_scale, ( double ) m_rule.get( "max_scale", 1.0 ) );
	const real_t scale =
		( real_t ) ( min_scale + ( max_scale - min_scale ) * randomUnit( key ^ UINT64_C( 0x349519 ) ) );
	const real_t	  yaw = ( bool ) m_rule.get( "random_yaw", true )
								? ( real_t ) ( randomUnit( key ^ UINT64_C( 0x8c815 ) ) * 6.283185307179586 )
								: 0.0f;
	const real_t	  align = ( real_t ) CLAMP( ( double ) m_rule.get( "normal_align", 0.0 ), 0.0, 1.0 );
	const Quaternion  align_q = Quaternion( Vector3( 0, 1, 0 ), normal );
	const Basis		  basis = Basis( Quaternion().slerp( align_q, align ) ) * Basis( Vector3( 0, 1, 0 ), yaw );
	const Transform3D transform( basis.scaled( Vector3( scale, scale, scale ) ),
								 Vector3( ( real_t ) x, height, ( real_t ) z ) );
	m_transforms.push_back( transform );
	++m_nAdded;
	++m_nRegionAdded;
	if ( m_nAdded > m_nJobCap || m_nRegionAdded > m_nRegionCap || ( int64_t ) m_nAdded * 96 > m_nMaxScratchBytes ) {
		m_error = "Foliage placement or editor scratch budget exceeded";
		return false;
	}
	return true;
}

bool Terrain3DFoliageGenerator::finishRule(
	void ) {
	Dictionary batch;
	batch["region"] = m_regions[m_nRegion];
	batch["mesh_id"] = m_rule.get( "mesh_id", -1 );
	batch["owner"] = m_rule.get( "owner", ( int64_t ) 0 );
	batch["transforms"] = m_transforms;
	m_batches.push_back( batch );
	m_transforms.clear();
	return true;
}

Dictionary Terrain3DFoliageGenerator::step(
	int nBudget ) {
	if ( !m_bRunning ) {
		return status();
	}
	nBudget = CLAMP( nBudget, 1, 65536 );
	while ( nBudget > 0 && m_nRegion < m_regions.size() ) {
		if ( m_nRule >= m_rules.size() ) {
			++m_nRegion;
			m_nRule = 0;
			m_nRegionAdded = 0;
			continue;
		}
		if ( m_nCandidate == 0 && m_nGridWidth == 0 && !prepareRule() ) {
			m_bFailed = true;
			break;
		}
		if ( m_nCandidate >= m_nCandidateCount ) {
			if ( Vector2i( m_rule.get( "region", Vector2i( INT32_MAX, INT32_MAX ) ) ) ==
				 Vector2i( m_regions[m_nRegion] ) ) {
				finishRule();
			}
			++m_nRule;
			m_nCandidate = 0;
			m_nGridWidth = 0;
			continue;
		}
		if ( !sampleCandidate() ) {
			m_bFailed = true;
			break;
		}
		++m_nCandidate;
		--nBudget;
	}
	if ( m_bFailed || m_nRegion >= m_regions.size() ) {
		m_bRunning = false;
	}
	return status();
}
void Terrain3DFoliageGenerator::cancel(
	void ) {
	m_bRunning = false;
	m_bFailed = false;
	m_pTerrain = nullptr;
	m_rules.clear();
	m_regions.clear();
	m_batches.clear();
	m_transforms.clear();
	m_nCandidate = 0;
	m_nGridWidth = 0;
	m_nRegion = 0;
	m_nRule = 0;
}
Array Terrain3DFoliageGenerator::takeBatches(
	void ) {
	if ( m_bRunning || m_bFailed || !m_error.is_empty() ) {
		return Array();
	}
	Array result = m_batches;
	m_batches.clear();
	return result;
}
Dictionary Terrain3DFoliageGenerator::status(
	void ) const {
	Dictionary result;
	result["running"] = m_bRunning;
	result["ready"] = !m_bRunning && !m_bFailed && m_error.is_empty() && !m_batches.is_empty();
	result["error"] = m_error;
	result["instances"] = m_nAdded;
	result["region_index"] = m_nRegion;
	result["region_count"] = m_regions.size();
	result["progress"] =
		m_regions.is_empty()
			? 0.0
			: CLAMP( ( double ) ( m_nRegion + ( double ) m_nRule / MAX( 1, m_rules.size() ) ) / m_regions.size(),
					 0.0,
					 1.0 );
	return result;
}
void Terrain3DFoliageGenerator::_bind_methods(
	void ) {
	ClassDB::bind_method( D_METHOD( "start", "terrain", "config", "regions" ), &Terrain3DFoliageGenerator::start );
	ClassDB::bind_method( D_METHOD( "step", "budget" ), &Terrain3DFoliageGenerator::step, DEFVAL( 2048 ) );
	ClassDB::bind_method( D_METHOD( "cancel" ), &Terrain3DFoliageGenerator::cancel );
	ClassDB::bind_method( D_METHOD( "take_batches" ), &Terrain3DFoliageGenerator::takeBatches );
	ClassDB::bind_method( D_METHOD( "status" ), &Terrain3DFoliageGenerator::status );
}
