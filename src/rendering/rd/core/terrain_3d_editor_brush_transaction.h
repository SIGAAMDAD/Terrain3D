// Copyright (c) 2026 Terrain3D contributors.
// BRP-04: bounded editor-only transient stroke contract.
#ifndef TERRAIN3D_EDITOR_BRUSH_TRANSACTION_H
#define TERRAIN3D_EDITOR_BRUSH_TRANSACTION_H

#include <stdint.h>
#include <math.h>
#include <string.h>

// No Godot references or heap allocations; GPU resource ownership belongs to
// Terrain3DRenderCoordinator / Terrain3DRdContext.
#define T3D_EDITOR_BRUSH_MAX_TILES ( 256u )
#define T3D_EDITOR_BRUSH_MAX_COMMANDS ( 1024u )
#define T3D_EDITOR_BRUSH_TILE_EDGE ( 16u )
#define T3D_EDITOR_BRUSH_MAX_TRANSFER_BYTES ( T3D_EDITOR_BRUSH_TILE_EDGE * T3D_EDITOR_BRUSH_TILE_EDGE * 4u * T3D_EDITOR_BRUSH_MAX_TILES )

typedef enum t3dEditorBrushStrokeState_e : uint8_t
{
	T3D_EDITOR_BRUSH_IDLE = 0u,
	T3D_EDITOR_BRUSH_ACTIVE,
	T3D_EDITOR_BRUSH_SEALED,
	T3D_EDITOR_BRUSH_READBACK_PENDING,
	T3D_EDITOR_BRUSH_READBACK_READY,
	T3D_EDITOR_BRUSH_CPU_COMMITTING,
	T3D_EDITOR_BRUSH_CPU_COMMITTED,
	T3D_EDITOR_BRUSH_CANCELED,
	T3D_EDITOR_BRUSH_FAILED
} t3dEditorBrushStrokeState_t;

typedef enum t3dEditorBrushMapEncoding_e : uint8_t
{
	T3D_EDITOR_BRUSH_HEIGHT_R16 = 0u,
	T3D_EDITOR_BRUSH_CONTROL_RF_BITS = 1u,
	T3D_EDITOR_BRUSH_COLOR_RGBA8 = 2u
} t3dEditorBrushMapEncoding_t;

typedef struct t3dEditorBrushTile_s
{
	int32_t m_nRegionX = 0;
	int32_t m_nRegionY = 0;
	uint64_t m_nSourceRevision = 0u;
	uint32_t m_nByteOffset = 0u;
	uint16_t m_nMinX = 0u;
	uint16_t m_nMinY = 0u;
	uint16_t m_nWidth = 0u;
	uint16_t m_nHeight = 0u;
	uint16_t m_nRowStrideBytes = 0u;
	uint8_t m_nEncoding = T3D_EDITOR_BRUSH_HEIGHT_R16;
	uint8_t m_nReserved = 0u;
} t3dEditorBrushTile_t;

typedef struct t3dEditorBrushCommand_s
{
	uint64_t m_nStrokeId = 0u;
	uint32_t m_nOwnerGeneration = 0u;
	uint32_t m_nDeviceGeneration = 0u;
	uint32_t m_nSequence = 0u;
	int32_t m_nRegionX = 0;
	int32_t m_nRegionY = 0;
	uint32_t m_nMapType = 0u;
	uint32_t m_nTool = 0u;
	uint32_t m_nOperation = 0u;
	uint32_t m_nBrushMaskGeneration = 0u;
	float m_fCenterX = 0.f;
	float m_fCenterZ = 0.f;
	float m_fRotation = 0.f;
	float m_fRadius = 0.f;
	float m_fStrength = 0.f;
	float m_fSelectedHeight = 0.f;
	uint32_t m_nFlags = 0u;
    // GPU producer parameters; the editor supplies exact sampled mask texels.
    float m_fVertexSpacing = 1.f;
    float m_fRoughness = 0.f;
    float m_fCenterHeight = 0.f;
    uint32_t m_nMaskWidth = 0u;
    uint32_t m_nMaskHeight = 0u;
    uint32_t m_nPackedColor = 0u;
    uint32_t m_nTileCountAtEnqueue = 0u;
    uint32_t m_nAssetId = 0u;
    uint32_t m_nUvRotation = 0u;
    uint32_t m_nUvScale = 0u;
    float m_fGradientX1 = 0.f, m_fGradientY1 = 0.f, m_fGradientZ1 = 0.f;
    float m_fGradientX2 = 0.f, m_fGradientY2 = 0.f, m_fGradientZ2 = 0.f;
    float m_fMotionX = 0.f, m_fMotionZ = 0.f;
} t3dEditorBrushCommand_t;

class Terrain3DEditorBrushTransaction
{
private:
	t3dEditorBrushTile_t m_tiles[T3D_EDITOR_BRUSH_MAX_TILES]{};
	t3dEditorBrushCommand_t m_commands[T3D_EDITOR_BRUSH_MAX_COMMANDS]{};
	uint64_t m_nStrokeId = 0u;
	uint32_t m_nOwnerGeneration = 0u;
	uint32_t m_nDeviceGeneration = 0u;
	uint32_t m_nPayloadBytes = 0u;
	uint32_t m_nSequence = 0u;
	uint16_t m_nTileCount = 0u;
	t3dEditorBrushStrokeState_t m_state = T3D_EDITOR_BRUSH_IDLE;

public:
	static uint32_t bytesPerTexel( const uint8_t nEncoding )
	{
		if ( nEncoding == T3D_EDITOR_BRUSH_HEIGHT_R16 ) return 2u;
		if ( nEncoding == T3D_EDITOR_BRUSH_CONTROL_RF_BITS ||
			 nEncoding == T3D_EDITOR_BRUSH_COLOR_RGBA8 ) return 4u;
		return 0u;
	}

	// Stage one tile into a detached, complete 128x128 native byte plane.
	// Pure byte operation shared by owner publication and host parity fixtures.
	static bool stageTileBytes( const t3dEditorBrushTile_t &rTile,
		const uint8_t *pPayload, const uint32_t nPayloadBytes,
		const uint8_t *pBefore, uint8_t *pAfter, const uint32_t nMapBytes,
		bool &rChanged, bool &rHoleChanged )
	{
		const uint32_t nPixelBytes = bytesPerTexel( rTile.m_nEncoding );
		if ( !nPixelBytes || !pPayload || !pBefore || !pAfter ||
			!rTile.m_nWidth || !rTile.m_nHeight ||
			rTile.m_nMinX + rTile.m_nWidth > 128u ||
			rTile.m_nMinY + rTile.m_nHeight > 128u ||
			rTile.m_nRowStrideBytes != rTile.m_nWidth * nPixelBytes ||
			rTile.m_nByteOffset > nPayloadBytes ) return false;
		const uint32_t nTileBytes = ( uint32_t ) rTile.m_nRowStrideBytes * rTile.m_nHeight;
		if ( nTileBytes > nPayloadBytes - rTile.m_nByteOffset ||
			nMapBytes != 128u * 128u * nPixelBytes ) return false;
		for ( uint32_t y = 0u; y < rTile.m_nHeight; ++y )
		{
			const uint32_t nDstOffset = ( ( uint32_t ) rTile.m_nMinY + y ) * 128u * nPixelBytes +
				( uint32_t ) rTile.m_nMinX * nPixelBytes;
			const uint32_t nSrcOffset = rTile.m_nByteOffset + y * rTile.m_nRowStrideBytes;
			if ( memcmp( pBefore + nDstOffset, pPayload + nSrcOffset, rTile.m_nRowStrideBytes ) )
				rChanged = true;
			if ( rTile.m_nEncoding == T3D_EDITOR_BRUSH_CONTROL_RF_BITS && !rHoleChanged )
			{
				for ( uint32_t x = 0u; x < rTile.m_nWidth; ++x )
				{
					uint32_t before = 0u, after = 0u;
					memcpy( &before, pBefore + nDstOffset + x * 4u, 4u );
					memcpy( &after, pPayload + nSrcOffset + x * 4u, 4u );
					if ( ( ( before ^ after ) & 4u ) != 0u ) { rHoleChanged = true; break; }
				}
			}
			memcpy( pAfter + nDstOffset, pPayload + nSrcOffset, rTile.m_nRowStrideBytes );
		}
		return true;
	}

	void reset( void )
	{
		*this = Terrain3DEditorBrushTransaction{};
	}

	bool begin( const uint64_t nStrokeId, const uint32_t nOwnerGeneration, const uint32_t nDeviceGeneration )
	{
		if ( m_state != T3D_EDITOR_BRUSH_IDLE || !nStrokeId || !nOwnerGeneration || !nDeviceGeneration ) return false;
		m_nStrokeId = nStrokeId;
		m_nOwnerGeneration = nOwnerGeneration;
		m_nDeviceGeneration = nDeviceGeneration;
		m_state = T3D_EDITOR_BRUSH_ACTIVE;
		return true;
	}

	bool appendTile( const t3dEditorBrushTile_t &rTile )
	{
		if ( m_state != T3D_EDITOR_BRUSH_ACTIVE || m_nTileCount >= T3D_EDITOR_BRUSH_MAX_TILES ||
			 rTile.m_nReserved || !rTile.m_nWidth || !rTile.m_nHeight ||
			 rTile.m_nMinX + rTile.m_nWidth > 128u || rTile.m_nMinY + rTile.m_nHeight > 128u ) return false;
		const uint32_t nPixelSize = bytesPerTexel( rTile.m_nEncoding );
		if ( !nPixelSize || rTile.m_nRowStrideBytes != rTile.m_nWidth * nPixelSize ) return false;
		const uint32_t nBytes = rTile.m_nRowStrideBytes * rTile.m_nHeight;
		if ( rTile.m_nByteOffset != m_nPayloadBytes ||
			 nBytes > T3D_EDITOR_BRUSH_MAX_TRANSFER_BYTES - m_nPayloadBytes ) return false;
		for ( uint32_t i = 0u; i < m_nTileCount; ++i )
		{
			const t3dEditorBrushTile_t &rPrev = m_tiles[i];
			if ( rPrev.m_nRegionX != rTile.m_nRegionX || rPrev.m_nRegionY != rTile.m_nRegionY ) continue;
			if ( rPrev.m_nEncoding != rTile.m_nEncoding ||
				 rPrev.m_nSourceRevision != rTile.m_nSourceRevision ) return false;
			const bool bOverlap = rTile.m_nMinX < rPrev.m_nMinX + rPrev.m_nWidth &&
				rPrev.m_nMinX < rTile.m_nMinX + rTile.m_nWidth &&
				rTile.m_nMinY < rPrev.m_nMinY + rPrev.m_nHeight &&
				rPrev.m_nMinY < rTile.m_nMinY + rTile.m_nHeight;
			if ( bOverlap ) return false;
		}
		m_tiles[m_nTileCount++] = rTile;
		m_nPayloadBytes += nBytes;
		return true;
	}

	bool acceptCommand( const t3dEditorBrushCommand_t &rCommand )
	{
		if ( m_state != T3D_EDITOR_BRUSH_ACTIVE || rCommand.m_nStrokeId != m_nStrokeId ||
			 rCommand.m_nOwnerGeneration != m_nOwnerGeneration ||
			 rCommand.m_nDeviceGeneration != m_nDeviceGeneration ||
			 rCommand.m_nSequence != m_nSequence + 1u ||
			 m_nSequence >= T3D_EDITOR_BRUSH_MAX_COMMANDS || rCommand.m_nMapType > 2u ||
			 !isfinite( rCommand.m_fRadius ) || rCommand.m_fRadius <= 0.f ||
			 !isfinite( rCommand.m_fStrength ) || !isfinite( rCommand.m_fCenterX ) ||
			 !isfinite( rCommand.m_fCenterZ ) || !isfinite( rCommand.m_fRotation ) ||
			 !isfinite( rCommand.m_fSelectedHeight ) ||
             !isfinite( rCommand.m_fVertexSpacing ) || rCommand.m_fVertexSpacing <= 0.f ||
             !isfinite( rCommand.m_fRoughness ) || !isfinite( rCommand.m_fCenterHeight ) ||
             ( rCommand.m_nOperation == 4u &&
             ( !isfinite( rCommand.m_fGradientX1 ) || !isfinite( rCommand.m_fGradientY1 ) ||
               !isfinite( rCommand.m_fGradientZ1 ) || !isfinite( rCommand.m_fGradientX2 ) ||
               !isfinite( rCommand.m_fGradientY2 ) || !isfinite( rCommand.m_fGradientZ2 ) ||
               !isfinite( rCommand.m_fMotionX ) || !isfinite( rCommand.m_fMotionZ ) ) ) ) return false;
		bool bCovered = false;
		for ( uint32_t i = 0u; i < m_nTileCount; ++i )
		{
			const t3dEditorBrushTile_t &rTile = m_tiles[i];
			if ( rTile.m_nRegionX == rCommand.m_nRegionX && rTile.m_nRegionY == rCommand.m_nRegionY &&
				 rTile.m_nEncoding == rCommand.m_nMapType ) { bCovered = true; break; }
		}
		if ( !bCovered ) return false;
		if ( rCommand.m_nTileCountAtEnqueue && rCommand.m_nTileCountAtEnqueue != m_nTileCount ) return false;
        m_commands[m_nSequence] = rCommand;
        m_commands[m_nSequence].m_nTileCountAtEnqueue = m_nTileCount;
        ++m_nSequence;
		return true;
	}

	bool seal( void )
	{
		if ( m_state != T3D_EDITOR_BRUSH_ACTIVE || !m_nTileCount || !m_nSequence ) return false;
		m_state = T3D_EDITOR_BRUSH_SEALED;
		return true;
	}

	bool markReadbackPending( void )
	{
		if ( m_state != T3D_EDITOR_BRUSH_SEALED ) return false;
		m_state = T3D_EDITOR_BRUSH_READBACK_PENDING;
		return true;
	}

	bool acceptReadback( const uint64_t nStrokeId, const uint32_t nOwnerGeneration,
							 const uint32_t nDeviceGeneration, const uint32_t nBytes )
	{
		if ( m_state != T3D_EDITOR_BRUSH_READBACK_PENDING || nStrokeId != m_nStrokeId ||
			 nOwnerGeneration != m_nOwnerGeneration || nDeviceGeneration != m_nDeviceGeneration ||
			 nBytes != m_nPayloadBytes ) return false;
		m_state = T3D_EDITOR_BRUSH_READBACK_READY;
		return true;
	}

	bool claimReadbackForCommit( void )
	{
		if ( m_state != T3D_EDITOR_BRUSH_READBACK_READY ) return false;
		m_state = T3D_EDITOR_BRUSH_CPU_COMMITTING;
		return true;
	}

	bool markCpuCommitted( void )
	{
		if ( m_state != T3D_EDITOR_BRUSH_CPU_COMMITTING ) return false;
		m_state = T3D_EDITOR_BRUSH_CPU_COMMITTED;
		return true;
	}

	void cancel( void )
	{
		if ( m_state != T3D_EDITOR_BRUSH_CPU_COMMITTED && m_state != T3D_EDITOR_BRUSH_IDLE )
			m_state = T3D_EDITOR_BRUSH_CANCELED;
	}

	void fail( void )
	{
		if ( m_state != T3D_EDITOR_BRUSH_CPU_COMMITTED ) m_state = T3D_EDITOR_BRUSH_FAILED;
	}

	t3dEditorBrushStrokeState_t state( void ) const { return m_state; }
	uint64_t strokeId( void ) const { return m_nStrokeId; }
	uint32_t ownerGeneration( void ) const { return m_nOwnerGeneration; }
	uint32_t deviceGeneration( void ) const { return m_nDeviceGeneration; }
	uint32_t payloadBytes( void ) const { return m_nPayloadBytes; }
	uint32_t tileCount( void ) const { return m_nTileCount; }
	uint32_t commandCount( void ) const { return m_nSequence; }
	const t3dEditorBrushTile_t &tile( const uint32_t nIndex ) const { return m_tiles[nIndex]; }
	const t3dEditorBrushCommand_t &command( const uint32_t nIndex ) const { return m_commands[nIndex]; }
};

#endif // TERRAIN3D_EDITOR_BRUSH_TRANSACTION_H
