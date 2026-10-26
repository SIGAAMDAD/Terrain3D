// Content identity shared by cooked page reads and optional global source pack.
#ifndef TERRAIN3D_STREAM_RANGE_KEY_H
#define TERRAIN3D_STREAM_RANGE_KEY_H
#include <godot_cpp/variant/string.hpp>
#include <stdint.h>
using namespace godot;
struct t3dStreamRangeKey_t
{
	String	 m_path;
	uint64_t m_nModifiedTime = 0u;
	uint64_t m_nFileLength = 0u;
	uint64_t m_nSourceSignature = 0u;
	uint32_t m_nOffset = 0u;
	uint32_t m_nStoredBytes = 0u;
	uint32_t m_nDecodedBytes = 0u;
	uint32_t m_nChecksum = 0u;
	uint32_t m_nCompression = 0u;
	uint32_t m_nFormat = 0u;
	uint32_t m_nChannel = 0u;
	uint8_t	 m_nPriority = 3u; // Not part of content identity.
	bool	 operator==(
		const t3dStreamRangeKey_t &rOther ) const {
		return m_path == rOther.m_path && m_nModifiedTime == rOther.m_nModifiedTime &&
			   m_nFileLength == rOther.m_nFileLength && m_nSourceSignature == rOther.m_nSourceSignature &&
			   m_nOffset == rOther.m_nOffset && m_nStoredBytes == rOther.m_nStoredBytes &&
			   m_nDecodedBytes == rOther.m_nDecodedBytes && m_nChecksum == rOther.m_nChecksum &&
			   m_nCompression == rOther.m_nCompression && m_nFormat == rOther.m_nFormat &&
			   m_nChannel == rOther.m_nChannel;
	}
};

#endif
