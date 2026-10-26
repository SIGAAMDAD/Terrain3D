// Copyright (c) 2026 Terrain3D Contributors.
// Authoritative packed terrain pages. Immutable encoded channels + companion page metadata.
#ifndef TERRAIN3D_SHARED_STREAM_PACK_H
#define TERRAIN3D_SHARED_STREAM_PACK_H
#include "streaming/io/terrain_3d_stream_range_key.h"
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <EASTL/vector.h>

struct t3dBakedRegionPage_t
{
	String			m_virtualPath; // stable index name, never an individual filesystem file
	PackedByteArray m_encodedPage; // authoring-only in-memory serialized page
};

struct t3dSharedStreamPackRecord_t
{
	t3dStreamRangeKey_t m_key;
	PackedByteArray		m_bytes; // stored/encoded bytes, not a decoded Image
};

class Terrain3DSharedStreamPack
{
	struct Item
	{
		String	 m_path;
		uint64_t m_nSourceSignature = 0u;
		uint64_t m_nDataOffset = 0u;
		uint32_t m_nSourceOffset = 0u;
		uint32_t m_nStoredBytes = 0u;
		uint32_t m_nDecodedBytes = 0u;
		uint32_t m_nChecksum = 0u;
		uint32_t m_nCompression = 0u;
		uint32_t m_nFormat = 0u;
		uint32_t m_nChannel = 0u;
		uint64_t m_nHash = 0u;
	};
	struct PageMeta
	{
		String			m_path;
		uint64_t		m_nLogicalLength = 0u;
		PackedByteArray m_header; // original 1248-byte T3PG v7 header
	};
	eastl::vector< PageMeta > m_pages;
	String					  m_path;
	uint64_t				  m_nModified = 0u;
	uint64_t				  m_nLength = 0u;
	eastl::vector< Item >	  m_items;
	// Sorted indices retain original record order for identical lookup keys.
	eastl::vector< uint16_t > m_sortedItemIndices;
	// Internal open of an unpublished generation during the cooker verification.
	bool openDirect( const String &rPath );

public:
	void close(
		void ) {
		m_items.clear();
		m_sortedItemIndices.clear();
		m_pages.clear();
		m_path = String();
		m_nModified = 0u;
		m_nLength = 0u;
	}
	// Cold remount: adopt an already validated immutable package without
	// reopening and parsing its index a second time.
	void swapState( Terrain3DSharedStreamPack &rOther ) {
		m_items.swap( rOther.m_items );
		m_sortedItemIndices.swap( rOther.m_sortedItemIndices );
		m_pages.swap( rOther.m_pages );
		const String oldPath = m_path;
		m_path = rOther.m_path;
		rOther.m_path = oldPath;
		const uint64_t oldModified = m_nModified;
		m_nModified = rOther.m_nModified;
		rOther.m_nModified = oldModified;
		const uint64_t oldLength = m_nLength;
		m_nLength = rOther.m_nLength;
		rOther.m_nLength = oldLength;
	}
	bool isOpen(
		void ) const {
		return !m_path.is_empty();
	}
	const String &path(
		void ) const {
		return m_path;
	}
	size_t itemCount(
		void ) const {
		return m_items.size();
	}
	bool open( const String &rPath );
#ifdef T3D_TESTS_ENABLED
	// Stages 1..5 simulate interrupted cold publication before commit visibility.
	static void setPublicationFaultForTest( uint32_t stage );
#endif
	bool lookupPage( const String &rPath, PackedByteArray &rHeader, uint64_t &rLogicalLength ) const;
	bool resolve( const t3dStreamRangeKey_t &rKey, t3dStreamRangeKey_t &rPhysicalKey ) const;
	// Deterministic writer for validated immutable source bytes.
	static bool buildRecords( const String &rPath, const eastl::vector< t3dSharedStreamPackRecord_t > &rRecords );
	// Cold rebake directly from authored resources' serialized channel buffers.
	static bool buildBakedPages( const String &rPath, const eastl::vector< t3dBakedRegionPage_t > &rPages );
};
#endif
