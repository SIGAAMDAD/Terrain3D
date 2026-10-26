// Fixed-capacity RID -> dense-record index. This table is owned by the RD
// resource registry and is always accessed with the registry mutex held.
#ifndef TERRAIN3D_RD_RID_INDEX_H
#define TERRAIN3D_RD_RID_INDEX_H

#include <stdint.h>

// The table has a maximum load of 1/2, and backward-shift deletion avoids
// tombstone accumulation after prolonged resource churn.
template< uint32_t nResourceCapacity >
class Terrain3DRdRidIndex
{
public:
	static constexpr uint32_t BUCKET_COUNT = nResourceCapacity * 2u;
	static_assert( nResourceCapacity > 0u && ( BUCKET_COUNT & ( BUCKET_COUNT - 1u ) ) == 0u,
		"RID index requires a power-of-two resource capacity" );

private:
	struct Bucket
	{
		uint64_t m_nRid = 0u;
		uint32_t m_nIndexPlusOne = 0u;
	};
	Bucket m_buckets[BUCKET_COUNT]{};
	uint32_t m_nCount = 0u;

	static uint32_t hashRid( uint64_t nRid ) {
		nRid ^= nRid >> 33u;
		nRid *= UINT64_C( 0xff51afd7ed558ccd );
		nRid ^= nRid >> 33u;
		nRid *= UINT64_C( 0xc4ceb9fe1a85ec53 );
		nRid ^= nRid >> 33u;
		return ( uint32_t ) nRid & ( BUCKET_COUNT - 1u );
	}

	uint32_t findBucket( uint64_t nRid ) const {
		uint32_t nSlot = hashRid( nRid );
		for ( uint32_t n = 0u; n < BUCKET_COUNT; ++n ) {
			const Bucket &rBucket = m_buckets[nSlot];
			if ( rBucket.m_nIndexPlusOne == 0u ) {
				return BUCKET_COUNT;
			}
			if ( rBucket.m_nRid == nRid ) {
				return nSlot;
			}
			nSlot = ( nSlot + 1u ) & ( BUCKET_COUNT - 1u );
		}
		return BUCKET_COUNT;
	}

public:
	void clear() {
		m_nCount = 0u;
		for ( uint32_t i = 0u; i < BUCKET_COUNT; ++i ) {
			m_buckets[i].m_nIndexPlusOne = 0u;
		}
	}

	uint32_t find( uint64_t nRid ) const {
		const uint32_t nSlot = findBucket( nRid );
		return nSlot == BUCKET_COUNT ? UINT32_MAX : m_buckets[nSlot].m_nIndexPlusOne - 1u;
	}

	bool insert( uint64_t nRid, uint32_t nDenseIndex ) {
		if ( nDenseIndex >= nResourceCapacity || m_nCount >= nResourceCapacity ) {
			return false;
		}
		uint32_t nSlot = hashRid( nRid );
		for ( uint32_t n = 0u; n < BUCKET_COUNT; ++n ) {
			Bucket &rBucket = m_buckets[nSlot];
			if ( rBucket.m_nIndexPlusOne == 0u ) {
				rBucket.m_nRid = nRid;
				rBucket.m_nIndexPlusOne = nDenseIndex + 1u;
				++m_nCount;
				return true;
			}
			if ( rBucket.m_nRid == nRid ) {
				return false;
			}
			nSlot = ( nSlot + 1u ) & ( BUCKET_COUNT - 1u );
		}
		return false;
	}

	bool updateIndex( uint64_t nRid, uint32_t nDenseIndex ) {
		const uint32_t nSlot = findBucket( nRid );
		if ( nSlot == BUCKET_COUNT || nDenseIndex >= nResourceCapacity ) {
			return false;
		}
		m_buckets[nSlot].m_nIndexPlusOne = nDenseIndex + 1u;
		return true;
	}

	bool erase( uint64_t nRid ) {
		uint32_t nHole = findBucket( nRid );
		if ( nHole == BUCKET_COUNT ) {
			return false;
		}
		uint32_t nScan = ( nHole + 1u ) & ( BUCKET_COUNT - 1u );
		while ( m_buckets[nScan].m_nIndexPlusOne != 0u ) {
			const uint32_t nHome = hashRid( m_buckets[nScan].m_nRid );
			// Move an entry backward only if the hole occurs on its probe path.
			if ( ( ( nScan - nHome ) & ( BUCKET_COUNT - 1u ) ) >=
				 ( ( nScan - nHole ) & ( BUCKET_COUNT - 1u ) ) ) {
				m_buckets[nHole] = m_buckets[nScan];
				nHole = nScan;
			}
			nScan = ( nScan + 1u ) & ( BUCKET_COUNT - 1u );
		}
		m_buckets[nHole].m_nIndexPlusOne = 0u;
		--m_nCount;
		return true;
	}
};

#endif
