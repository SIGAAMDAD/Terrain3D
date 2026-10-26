#include "data/cooking/terrain_3d_raster_hierarchy_builder.h"
#include "data/cooking/terrain_3d_raster_categorical_pick.h"
#include "diagnostics/core/terrain_3d_streaming_profiler.h"
#include "data/persistence/terrain_3d_region_codec.h"
#include <string.h>
#include <math.h>

using namespace godot;

uint16_t Terrain3DRasterHierarchyBuilder::nodeId(
	const uint8_t nLevel,
	const uint8_t nX,
	const uint8_t nY ) {
	if ( nLevel > 5u || nX >= ( 1u << nLevel ) || nY >= ( 1u << nLevel ) ) {
		return UINT16_MAX;
	}
	uint32_t offset = 0u;
	for ( uint8_t i = 0u; i < nLevel; ++i ) {
		offset += 1u << ( 2u * i );
	}
	return ( uint16_t ) ( offset + ( uint32_t ) nY * ( 1u << nLevel ) + nX );
}

void Terrain3DRasterHierarchyBuilder::clear(
	void ) {
	m_nodes.clear();
	m_payloads.clear();
	m_nGeneration = 1u;
}

const t3dRasterHierarchyNode_t *Terrain3DRasterHierarchyBuilder::getNode(
	const uint8_t nLevel,
	const uint8_t nX,
	const uint8_t nY ) const {
	const auto it = m_nodes.find( nodeId( nLevel, nX, nY ) );
	return it == m_nodes.end() ? NULL : &it->second;
}

const t3dRasterHierarchyNode_t *Terrain3DRasterHierarchyBuilder::getNodeById(
	const uint16_t nId ) const {
	const auto it = m_nodes.find( nId );
	return it == m_nodes.end() ? NULL : &it->second;
}

const t3dRasterHierarchyPayload_t *Terrain3DRasterHierarchyBuilder::getPayload(
	const uint8_t nLevel,
	const uint8_t nX,
	const uint8_t nY ) const {
	const auto it = m_payloads.find( nodeId( nLevel, nX, nY ) );
	return it == m_payloads.end() ? NULL : &it->second;
}

void Terrain3DRasterHierarchyBuilder::rebuildParents(
	uint8_t x,
	uint8_t y ) {
	Terrain3DStreamingProfileWorkScope compared( T3D_SP_H04_RASTER_COOK );
	for ( int level = 4; level >= 0; --level ) {
		x >>= 1u;
		y >>= 1u;
		t3dRasterHierarchyNode_t parent;
		parent.m_fMinHeight = 1e30f;
		parent.m_fMaxHeight = -1e30f;
		uint64_t color_sums[4] = {};
		uint32_t dominant_count = 0u;
		for ( uint8_t dy = 0u; dy < 2u; ++dy ) {
			for ( uint8_t dx = 0u; dx < 2u; ++dx ) {
				const t3dRasterHierarchyNode_t *const pChild =
					getNode( ( uint8_t ) ( level + 1 ), ( uint8_t ) ( x * 2 + dx ), ( uint8_t ) ( y * 2 + dy ) );
				if ( !pChild || pChild->m_nLeafCount == 0u ) {
					continue;
				}
				parent.m_fMinHeight = MIN( parent.m_fMinHeight, pChild->m_fMinHeight );
				parent.m_fMaxHeight = MAX( parent.m_fMaxHeight, pChild->m_fMaxHeight );
				if ( pChild->m_nLeafCount > dominant_count ) {
					parent.m_nControl = pChild->m_nControl; // categorical: never average packed IDs
					dominant_count = pChild->m_nLeafCount;
				}
				for ( uint32_t channel = 0u; channel < 4u; ++channel ) {
					color_sums[channel] += ( uint64_t ) pChild->m_szColor[channel] * pChild->m_nLeafCount;
				}
				parent.m_nLeafCount += pChild->m_nLeafCount;
				parent.m_nChildMask |= ( uint8_t ) ( 1u << ( dy * 2u + dx ) );
			}
		}
		const uint16_t id = nodeId( ( uint8_t ) level, x, y );
		if ( parent.m_nLeafCount == 0u ) {
			m_nodes.erase( id );
			m_payloads.erase( id );
			continue;
		}
		for ( uint32_t c = 0u; c < 4u; ++c ) {
			parent.m_szColor[c] = ( uint8_t ) ( color_sums[c] / parent.m_nLeafCount );
		}
		parent.m_nRevision = ++m_nGeneration;
		t3dRasterHierarchyPayload_t payload;
		const uint16_t				flat = t3dRegionCodecEncodeHeightU16( 0.0f );
		for ( uint32_t py = 0u; py < 32u; ++py ) {
			for ( uint32_t px = 0u; px < 32u; ++px ) {
				const uint8_t	  childX = ( uint8_t ) ( x * 2u + px / 16u );
				const uint8_t	  childY = ( uint8_t ) ( y * 2u + py / 16u );
				const auto *const pChild = getPayload( ( uint8_t ) ( level + 1 ), childX, childY );
				const uint32_t	  at = py * 32u + px;
				if ( !pChild ) {
					payload.m_szHeight[at] = flat;
					payload.m_szControl[at] = T3D_REGION_DEFAULT_CONTROL;
					payload.m_szColor[at * 4u + 0u] = 255u;
					payload.m_szColor[at * 4u + 1u] = 255u;
					payload.m_szColor[at * 4u + 2u] = 255u;
					payload.m_szColor[at * 4u + 3u] = 128u;
					continue;
				}
				uint32_t heightSum = 0u;
				uint32_t colorSum[4] = {};
				uint32_t controls[4] = {};
				for ( uint32_t oy = 0u; oy < 2u; ++oy ) {
					for ( uint32_t ox = 0u; ox < 2u; ++ox ) {
						const uint32_t sourceX = ( px % 16u ) * 2u + ox;
						const uint32_t sourceY = ( py % 16u ) * 2u + oy;
						const uint32_t source = sourceY * 32u + sourceX;
						heightSum += pChild->m_szHeight[source];
						controls[oy * 2u + ox] = pChild->m_szControl[source];
						for ( uint32_t channel = 0u; channel < 4u; ++channel ) {
							colorSum[channel] += pChild->m_szColor[source * 4u + channel];
						}
					}
				}
				payload.m_szHeight[at] = ( uint16_t ) ( ( heightSum + 2u ) / 4u );
				compared.add( 16u );
				payload.m_szControl[at] = t3dRasterCategoricalPick( controls, 4u );
				for ( uint32_t channel = 0u; channel < 4u; ++channel ) {
					payload.m_szColor[at * 4u + channel] = ( uint8_t ) ( ( colorSum[channel] + 2u ) / 4u );
				}
			}
		}
		// Every exact fine sample maps to a nearest-cell child sample.
		// Triangle inequality: fine->child error + child->parent difference.
		for ( uint8_t dy = 0u; dy < 2u; ++dy ) {
			for ( uint8_t dx = 0u; dx < 2u; ++dx ) {
				const uint8_t childX = ( uint8_t ) ( x * 2u + dx );
				const uint8_t childY = ( uint8_t ) ( y * 2u + dy );
				const t3dRasterHierarchyNode_t *const pChildNode = getNode( ( uint8_t ) ( level + 1 ), childX, childY );
				const t3dRasterHierarchyPayload_t *const pChildPayload = getPayload( ( uint8_t ) ( level + 1 ), childX, childY );
				if ( !pChildNode || !pChildPayload ) {
					continue;
				}
				for ( uint32_t py = 0u; py < 32u; ++py ) {
					for ( uint32_t px = 0u; px < 32u; ++px ) {
						const uint16_t fine = pChildPayload->m_szHeight[py * 32u + px];
						const uint16_t coarse = payload.m_szHeight[( dy * 16u + py / 2u ) * 32u + dx * 16u + px / 2u];
						const float error = pChildNode->m_fMaxHeightErrorM +
							fabsf( t3dRegionCodecDecodeHeightU16( fine ) - t3dRegionCodecDecodeHeightU16( coarse ) );
						parent.m_fMaxHeightErrorM = MAX( parent.m_fMaxHeightErrorM, error );
					}
				}
			}
		}
		m_nodes[id] = parent;
		m_payloads[id] = payload;
	}
}

void Terrain3DRasterHierarchyBuilder::updateRegion(
	const Vector2i				   &rRegion,
	const Terrain3DRasterTileStore &rTiles ) {
	T3D_STREAM_PROFILE_SCOPE( T3D_SP_H04_RASTER_COOK );
	Terrain3DStreamingProfileWorkScope comparisons( T3D_SP_H04_RASTER_COOK );
	if ( rRegion.x < -16 || rRegion.x >= 16 || rRegion.y < -16 || rRegion.y >= 16 ) {
		return;
	}
	const uint8_t				x = ( uint8_t ) ( rRegion.x + 16 );
	const uint8_t				y = ( uint8_t ) ( rRegion.y + 16 );
	t3dRasterHierarchyNode_t	leaf;
	t3dRasterHierarchyPayload_t leafPayload;
	leaf.m_fMinHeight = 1e30f;
	leaf.m_fMaxHeight = -1e30f;
	leaf.m_nLeafCount = 1u;
	uint64_t color_sum[4] = {};
	uint32_t control_counts[32] = {};
	uint32_t representative_words[32] = {};
	for ( uint8_t ty = 0u; ty < 4u; ++ty ) {
		for ( uint8_t tx = 0u; tx < 4u; ++tx ) {
			PackedByteArray height;
			PackedByteArray control;
			PackedByteArray color;
			if ( !rTiles.reconstructTile( t3dRasterTileId( rRegion, TYPE_HEIGHT, tx, ty ), height ) ||
				 !rTiles.reconstructTile( t3dRasterTileId( rRegion, TYPE_CONTROL, tx, ty ), control ) ||
				 !rTiles.reconstructTile( t3dRasterTileId( rRegion, TYPE_COLOR, tx, ty ), color ) ) {
				return;
			}
			const uint8_t *const pHeight = height.ptr();
			const uint8_t *const pControl = control.ptr();
			const uint8_t *const pColor = color.ptr();
			for ( uint32_t qy = 0u; qy < 8u; ++qy ) {
				for ( uint32_t qx = 0u; qx < 8u; ++qx ) {
					uint32_t hSum = 0u;
					uint32_t cSum[4] = {};
					uint32_t categories[16] = {};
					for ( uint32_t sy = 0u; sy < 4u; ++sy ) {
						for ( uint32_t sx = 0u; sx < 4u; ++sx ) {
							const uint32_t p = ( qy * 4u + sy ) * 32u + qx * 4u + sx;
							uint16_t	   h;
							uint32_t	   control;
							memcpy( &h, pHeight + p * 2u, 2u );
							memcpy( &control, pControl + p * 4u, 4u );
							hSum += h;
							categories[sy * 4u + sx] = control;
							for ( uint32_t channel = 0u; channel < 4u; ++channel ) {
								cSum[channel] += pColor[p * 4u + channel];
							}
						}
					}
					const uint32_t p = ( ty * 8u + qy ) * 32u + tx * 8u + qx;
					leafPayload.m_szHeight[p] = ( uint16_t ) ( ( hSum + 8u ) / 16u );
					// Capture maximum exact-source discrepancy for this 4x4 block.
					const float approximation = t3dRegionCodecDecodeHeightU16( leafPayload.m_szHeight[p] );
					for ( uint32_t sy = 0u; sy < 4u; ++sy ) {
						for ( uint32_t sx = 0u; sx < 4u; ++sx ) {
							const uint32_t src = ( qy * 4u + sy ) * 32u + qx * 4u + sx;
							uint16_t encoded;
							memcpy( &encoded, pHeight + src * 2u, sizeof( encoded ) );
							leaf.m_fMaxHeightErrorM = MAX( leaf.m_fMaxHeightErrorM,
								fabsf( t3dRegionCodecDecodeHeightU16( encoded ) - approximation ) );
						}
					}
					comparisons.add( 32u );
					leafPayload.m_szControl[p] = t3dRasterCategoricalPick( categories, 16u );
					for ( uint32_t channel = 0u; channel < 4u; ++channel ) {
						leafPayload.m_szColor[p * 4u + channel] = ( uint8_t ) ( ( cSum[channel] + 8u ) / 16u );
					}
				}
			}
			for ( uint32_t pixel = 0u; pixel < 1024u; ++pixel ) {
				uint16_t encoded_height;
				memcpy( &encoded_height, pHeight + pixel * 2u, 2u );
				const float h = t3dRegionCodecDecodeHeightU16( encoded_height );
				leaf.m_fMinHeight = MIN( leaf.m_fMinHeight, h );
				leaf.m_fMaxHeight = MAX( leaf.m_fMaxHeight, h );
				uint32_t word;
				memcpy( &word, pControl + pixel * 4u, 4u );
				const uint32_t base = ( word >> 27u ) & 31u;
				if ( ++control_counts[base] == 1u ) {
					representative_words[base] = word;
				}
				for ( uint32_t c = 0u; c < 4u; ++c ) {
					color_sum[c] += pColor[pixel * 4u + c];
				}
			}
		}
	}
	uint32_t largest = 0u;
	for ( uint32_t i = 0u; i < 32u; ++i ) {
		if ( control_counts[i] > largest ) {
			largest = control_counts[i];
			leaf.m_nControl = representative_words[i];
		}
	}
	for ( uint32_t c = 0u; c < 4u; ++c ) {
		leaf.m_szColor[c] = ( uint8_t ) ( color_sum[c] / ( 128u * 128u ) );
	}
	leaf.m_nRevision = ++m_nGeneration;
	m_nodes[nodeId( 5u, x, y )] = leaf;
	m_payloads[nodeId( 5u, x, y )] = leafPayload;
	rebuildParents( x, y );
}

void Terrain3DRasterHierarchyBuilder::removeRegion(
	const Vector2i &rRegion ) {
	if ( rRegion.x < -16 || rRegion.x >= 16 || rRegion.y < -16 || rRegion.y >= 16 ) {
		return;
	}
	const uint8_t x = ( uint8_t ) ( rRegion.x + 16 );
	const uint8_t y = ( uint8_t ) ( rRegion.y + 16 );
	m_nodes.erase( nodeId( 5u, x, y ) );
	m_payloads.erase( nodeId( 5u, x, y ) );
	rebuildParents( x, y );
}

Dictionary Terrain3DRasterHierarchyBuilder::getStats(
	void ) const {
	Dictionary stats;
	stats["raster_hierarchy_nodes"] = ( int64_t ) m_nodes.size();
	stats["raster_hierarchy_raster_nodes"] = ( int64_t ) m_payloads.size();
	stats["raster_hierarchy_raster_bytes"] =
		( int64_t ) m_payloads.size() * ( int64_t ) sizeof( t3dRasterHierarchyPayload_t );
	stats["raster_hierarchy_generation"] = ( int64_t ) m_nGeneration;
	const t3dRasterHierarchyNode_t *const pRoot = getNode( 0u, 0u, 0u );
	stats["raster_hierarchy_regions"] = pRoot ? ( int64_t ) pRoot->m_nLeafCount : 0;
	stats["raster_hierarchy_min_height"] = pRoot ? pRoot->m_fMinHeight : 0.0f;
	stats["raster_hierarchy_max_height"] = pRoot ? pRoot->m_fMaxHeight : 0.0f;
	stats["raster_hierarchy_max_error_m"] = pRoot ? pRoot->m_fMaxHeightErrorM : 0.0f;
	return stats;
}
