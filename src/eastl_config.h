#ifndef TERRAIN3D_EASTL_CONFIG_H
#define TERRAIN3D_EASTL_CONFIG_H

#include "Terrain3DTypes.h"
#include "Terrain3DConstants.h"
#define TERRAIN3D_EASTL_CONFIGURED 1

#include "core/config/terrain_3d_compiler.h"
#include "core/utility/terrain_3d_sync.h"
#include "memory/core/terrain_3d_memory.h"
#include "memory/arena/terrain_3d_zone_allocator.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <new>

// EASTL is used only in Terrain3D internals. Public Godot APIs remain Godot types.
#define EASTL_EABASE_DISABLED		 1
#define EASTL_USER_DEFINED_ALLOCATOR 1
#define EASTL_EXCEPTIONS_ENABLED	 0
#define EASTL_RTTI_ENABLED			 0
#if defined( NDEBUG )
#define EASTL_DEBUG			 0
#define EASTL_ASSERT_ENABLED 0
#else
#define EASTL_DEBUG			 1
#define EASTL_ASSERT_ENABLED 1
#endif
#define EASTL_NAME_ENABLED 0
#define EASTL_DLL		   0
#define EASTL_API
#define EASTL_LOCAL
#define EASTL_DEBUGPARAMS_LEVEL		 0
#define EASTL_ALLOCATOR_DEFAULT_NAME "Terrain3D EASTL"
#define EASTL_NAME( x )				 x
#define EASTL_NAME_VAL( x )			 x

typedef struct t3dEastlAllocationHeader_s
{
	void				*m_pRaw = NULL;
	Terrain3DMemoryZone *m_pZone = NULL;
	size_t				 m_nSize = 0u;
	uint8_t				 m_nOrigin = TERRAIN3D_ALLOCATION_BOOTSTRAP;
} t3dEastlAllocationHeader_t;

#define s_nEastlAllocationHeaderSize ( sizeof( t3dEastlAllocationHeader_t ) )

class Terrain3DEastlAllocator
{
	Terrain3DMemoryZone			*m_pZone = NULL;
	t3dAllocatorOverflowPolicy_t m_eOverflowPolicy = TERRAIN3D_ALLOCATOR_OVERFLOW_STRICT;

	inline static Terrain3DAtomic< size_t > s_nLiveBytes{ 0u };
	inline static Terrain3DAtomic< size_t > s_nPeakBytes{ 0u };
	inline static Terrain3DAtomic< size_t > s_nLiveAllocations{ 0u };
	inline static Terrain3DAtomic< size_t > s_nTotalAllocations{ 0u };
	inline static Terrain3DAtomic< size_t > s_nFailedAllocations{ 0u };
	inline static Terrain3DAtomic< size_t > s_nOverflowAllocations{ 0u };
	inline static Terrain3DAtomic< size_t > s_nOverflowLiveBytes{ 0u };

	static void recordAllocation(
		const size_t  nSize,
		const uint8_t nOrigin ) {
		const size_t live = s_nLiveBytes.fetch_add( nSize, T3D_MEMORY_ORDER_RELAXED ) + nSize;
		s_nLiveAllocations.fetch_add( 1u, T3D_MEMORY_ORDER_RELAXED );
		s_nTotalAllocations.fetch_add( 1u, T3D_MEMORY_ORDER_RELAXED );
		if ( nOrigin == TERRAIN3D_ALLOCATION_OVERFLOW ) {
			s_nOverflowAllocations.fetch_add( 1u, T3D_MEMORY_ORDER_RELAXED );
			s_nOverflowLiveBytes.fetch_add( nSize, T3D_MEMORY_ORDER_RELAXED );
		}
		size_t peak = s_nPeakBytes.load( T3D_MEMORY_ORDER_RELAXED );
		while (
			live > peak &&
			!s_nPeakBytes.compare_exchange_weak( peak, live, T3D_MEMORY_ORDER_RELAXED, T3D_MEMORY_ORDER_RELAXED ) ) {
		}
	}

	EA_FORCE_INLINE static void recordFailure(
		void ) {
		s_nFailedAllocations.fetch_add( 1u, T3D_MEMORY_ORDER_RELAXED );
	}

public:
	EA_FORCE_INLINE Terrain3DEastlAllocator(
		const char * = EASTL_ALLOCATOR_DEFAULT_NAME ) {
	}
	Terrain3DEastlAllocator( const Terrain3DEastlAllocator & ) = default;
	EA_FORCE_INLINE Terrain3DEastlAllocator(
		const Terrain3DEastlAllocator &rOther,
		const char * ) :
		m_pZone( rOther.m_pZone ),
		m_eOverflowPolicy( rOther.m_eOverflowPolicy ) {
	}
	Terrain3DEastlAllocator						  &operator=( const Terrain3DEastlAllocator & ) = default;
	EA_FORCE_INLINE static Terrain3DEastlAllocator bootstrapAllocator(
		void ) {
		return Terrain3DEastlAllocator();
	}
	EA_FORCE_INLINE static Terrain3DEastlAllocator zoneAllocator(
		Terrain3DMemoryZone *const		   pZone,
		const t3dAllocatorOverflowPolicy_t eOverflowPolicy = TERRAIN3D_ALLOCATOR_OVERFLOW_STRICT ) {
		Terrain3DEastlAllocator allocator;
		allocator.m_pZone = pZone;
		allocator.m_eOverflowPolicy = eOverflowPolicy;
		return allocator;
	}
	EA_FORCE_INLINE Terrain3DMemoryZone *zone(
		void ) const {
		return m_pZone;
	}
	EA_FORCE_INLINE bool isBootstrap(
		void ) const {
		return m_pZone == NULL;
	}
	EA_FORCE_INLINE const char *getName(
		void ) const {
		return EASTL_ALLOCATOR_DEFAULT_NAME;
	}
	EA_FORCE_INLINE void setName(
		const char * ) {
	}

	EA_FORCE_INLINE void *allocate(
		const size_t n,
		int = 0 ) {
		return allocate( n, T3D_EASTL_DEFAULT_ALIGNMENT, 0, 0 );
	}
	bool canAllocate(
		const size_t n,
		size_t		 alignment = T3D_EASTL_DEFAULT_ALIGNMENT,
		const size_t alignment_offset = 0u ) const {
		if ( alignment < alignof( void * ) ) {
			alignment = alignof( void * );
		}
		if ( n == 0u || ( alignment & ( alignment - 1u ) ) != 0u ) {
			return false;
		}
		const size_t header_size = s_nEastlAllocationHeaderSize;
		const size_t alignment_slack = alignment - 1u;
		if ( n > SIZE_MAX - header_size - alignment_slack ) {
			return false;
		}
		if ( !m_pZone ) {
			if ( !g_t3dMemoryAccount ) {
				return true;
			}
			const auto	   stats = g_t3dMemoryAccount->stats();
			const uint64_t overhead = header_size + alignment_slack + sizeof( t3dNativeMemoryHeader );
			const uint64_t available = stats.limit - stats.committed;
			return overhead <= available && n <= available - overhead;
		}
		const size_t total = n + header_size + alignment_slack;
		if ( m_pZone->canAllocate( total, alignment ) ) {
			return true;
		}
		return m_eOverflowPolicy == TERRAIN3D_ALLOCATOR_OVERFLOW_HEAP;
	}
	void *allocate(
		const size_t n,
		size_t		 alignment,
		const size_t alignment_offset,
		int = 0 ) {
		if ( alignment < alignof( void * ) ) {
			alignment = alignof( void * );
		}
		if ( n == 0u ) {
			return NULL;
		}
		if ( ( alignment & ( alignment - 1u ) ) != 0u ) {
			recordFailure();
			return NULL;
		}
		const size_t header_size = s_nEastlAllocationHeaderSize;
		const size_t alignment_slack = alignment - 1u;
		if ( n > SIZE_MAX - header_size - alignment_slack ) {
			recordFailure();
			return NULL;
		}
		const size_t			   total = n + header_size + alignment_slack;
		Terrain3DMemoryZone *const pZone = m_pZone;
		void					  *pRaw = pZone ? pZone->allocate( total, alignment ) : t3dMemoryAllocate( total );
		uint8_t					   origin = pZone ? TERRAIN3D_ALLOCATION_ZONE : TERRAIN3D_ALLOCATION_BOOTSTRAP;
		if ( !pRaw && pZone && m_eOverflowPolicy == TERRAIN3D_ALLOCATOR_OVERFLOW_HEAP ) {
			pRaw = t3dMemoryAllocate( total );
			origin = TERRAIN3D_ALLOCATION_OVERFLOW;
		}
		if ( !pRaw ) {
			recordFailure();
			return NULL;
		}
		const uintptr_t begin = ( uintptr_t ) pRaw + header_size;
		const uintptr_t effective_offset = ( uintptr_t ) ( alignment_offset & alignment_slack );
		uintptr_t		aligned = ( begin + effective_offset + alignment_slack ) & ~( uintptr_t ) alignment_slack;
		aligned -= effective_offset;
		t3dEastlAllocationHeader_t header;
		header.m_pRaw = pRaw;
		header.m_pZone = pZone;
		header.m_nSize = n;
		header.m_nOrigin = origin;
		memcpy( ( void * ) ( aligned - header_size ), &header, header_size );
		recordAllocation( n, origin );
		t3dRecordRuntimeEastlAllocation( n );
		return ( void * ) aligned;
	}

	void deallocate(
		void *const p,
		size_t = 0 ) {
		if ( !p ) {
			return;
		}
		t3dEastlAllocationHeader_t header;
		memcpy( &header,
				( unsigned char * ) p - sizeof( t3dEastlAllocationHeader_t ),
				sizeof( t3dEastlAllocationHeader_t ) );
		s_nLiveBytes.fetch_sub( header.m_nSize, T3D_MEMORY_ORDER_RELAXED );
		s_nLiveAllocations.fetch_sub( 1u, T3D_MEMORY_ORDER_RELAXED );
		if ( header.m_nOrigin == TERRAIN3D_ALLOCATION_OVERFLOW ) {
			s_nOverflowLiveBytes.fetch_sub( header.m_nSize, T3D_MEMORY_ORDER_RELAXED );
		}
		if ( header.m_nOrigin == TERRAIN3D_ALLOCATION_ZONE && header.m_pZone ) {
			header.m_pZone->deallocate( header.m_pRaw );
		} else {
			t3dMemoryRelease( header.m_pRaw );
		}
	}

	void set_name(
		const char *pszAllocatorName ) {
	}

	const char *get_name(
		void ) {
		return "T3DEASTLAllocator";
	}

	static t3dEastlAllocationStats_t getAllocationStats(
		void ) {
		t3dEastlAllocationStats_t stats;
		stats.m_nLiveBytes = s_nLiveBytes.load( T3D_MEMORY_ORDER_RELAXED );
		stats.m_nPeakBytes = s_nPeakBytes.load( T3D_MEMORY_ORDER_RELAXED );
		stats.m_nLiveAllocations = s_nLiveAllocations.load( T3D_MEMORY_ORDER_RELAXED );
		stats.m_nTotalAllocations = s_nTotalAllocations.load( T3D_MEMORY_ORDER_RELAXED );
		stats.m_nFailedAllocations = s_nFailedAllocations.load( T3D_MEMORY_ORDER_RELAXED );
		stats.m_nOverflowAllocations = s_nOverflowAllocations.load( T3D_MEMORY_ORDER_RELAXED );
		stats.m_nOverflowLiveBytes = s_nOverflowLiveBytes.load( T3D_MEMORY_ORDER_RELAXED );
		return stats;
	}

	friend bool operator==(
		const Terrain3DEastlAllocator &rLeft,
		const Terrain3DEastlAllocator &rRight ) {
		return rLeft.m_pZone == rRight.m_pZone && rLeft.m_eOverflowPolicy == rRight.m_eOverflowPolicy;
	}
	friend bool operator!=(
		const Terrain3DEastlAllocator &rLeft,
		const Terrain3DEastlAllocator &rRight ) {
		return !( rLeft == rRight );
	}
};
#define EASTLAllocatorType Terrain3DEastlAllocator
EA_FORCE_INLINE static Terrain3DEastlAllocator *T3DGetDefaultEastlAllocator(
	void ) {
	static Terrain3DEastlAllocator s_allocator;
	return &s_allocator;
}
#define EASTLAllocatorDefault T3DGetDefaultEastlAllocator

#if defined( _MSC_VER )
#define EA_COMPILER_MSVC	1
#define EA_COMPILER_VERSION _MSC_VER
#elif defined( __clang__ )
#define EA_COMPILER_CLANG	1
#define EA_COMPILER_VERSION ( __clang_major__ * 100 + __clang_minor__ )
#elif defined( __GNUC__ )
#define EA_COMPILER_GNUC	1
#define EA_COMPILER_VERSION ( __GNUC__ * 1000 + __GNUC_MINOR__ )
#endif

#if defined( _WIN32 )
#define EA_PLATFORM_WINDOWS	  1
#define EA_PLATFORM_MICROSOFT 1
#elif defined( __APPLE__ )
#define EA_PLATFORM_APPLE 1
#define EA_PLATFORM_OSX	  1
#elif defined( __linux__ )
#define EA_PLATFORM_LINUX 1
#define EA_PLATFORM_UNIX  1
#endif

#if INTPTR_MAX == INT64_MAX
#define EA_PLATFORM_PTR_SIZE			 8
#define EA_PLATFORM_WORD_SIZE			 8
#define EA_PLATFORM_MIN_MALLOC_ALIGNMENT 16
#else
#define EA_PLATFORM_PTR_SIZE			 4
#define EA_PLATFORM_WORD_SIZE			 4
#define EA_PLATFORM_MIN_MALLOC_ALIGNMENT 8
#endif

#if __cplusplus >= 202002L
#define EA_COMPILER_CPP20_ENABLED 1
#endif
#if __cplusplus >= 201703L
#define EA_COMPILER_CPP17_ENABLED 1
#endif
#if __cplusplus >= 201402L
#define EA_COMPILER_CPP14_ENABLED 1
#endif
#if __cplusplus >= 201103L
#define EA_COMPILER_CPP11_ENABLED 1
#endif

#define EA_PRAGMA_ONCE_SUPPORTED 1
#define EA_TSAN_ENABLED			 0
#define EA_NOEXCEPT				 noexcept
#define EA_NOEXCEPT_IF( x )		 noexcept( x )
#define EA_NOEXCEPT_EXPR( x )	 noexcept( x )
#define EA_CPP14_CONSTEXPR		 constexpr
#define EA_CPP17_CONSTEXPR		 constexpr
#define EA_CPP20_CONSTEXPR		 constexpr
#define EA_CONSTEXPR			 constexpr
#define EA_CONSTEXPR_IF			 if constexpr
#define EA_OVERRIDE				 override
#define EA_FINAL				 final
#define EA_CONSTEXPR_OR_CONST	 constexpr
#if defined( _MSC_VER )
#define EA_GET_INSTRUCTION_POINTER() _ReturnAddress()
#else
#define EA_GET_INSTRUCTION_POINTER() __builtin_return_address( 0 )
#endif
#define EA_ALIGN_OF( type ) alignof( type )
#if defined( _MSC_VER )
#define EA_ALIGN( n )		 __declspec( align( n ) )
#define EA_PREFIX_ALIGN( n ) __declspec( align( n ) )
#define EA_POSTFIX_ALIGN( n )
#define EA_RESTRICT __restrict
#else
#define EA_ALIGN( n ) __attribute__( ( aligned( n ) ) )
#define EA_PREFIX_ALIGN( n )
#define EA_POSTFIX_ALIGN( n ) __attribute__( ( aligned( n ) ) )
#define EA_RESTRICT			  __restrict__
#endif
#define EA_PACKED
#define EA_MAY_ALIAS
#if defined( _MSC_VER )
#define EA_LIKELY( x )	 ( x )
#define EA_UNLIKELY( x ) ( x )
#define EA_ASSUME( x )	 __assume( x )
#else
#define EA_LIKELY( x )	 __builtin_expect( !!( x ), 1 )
#define EA_UNLIKELY( x ) __builtin_expect( !!( x ), 0 )
#define EA_ASSUME( x )                                                                                                 \
	do {                                                                                                               \
		if ( !( x ) ) {                                                                                                \
			__builtin_unreachable();                                                                                   \
		}                                                                                                              \
	} while ( 0 )
#endif
#define EA_UNUSED( x )			( void ) ( x )
#define EA_ANALYSIS_ASSUME( x ) EA_ASSUME( x )
#define EA_DISABLE_VC_WARNING( x )
#define EA_RESTORE_VC_WARNING()
#define EA_DISABLE_ALL_VC_WARNINGS()
#define EA_RESTORE_ALL_VC_WARNINGS()
#define EA_PUSH_VC_WARNING()
#define EA_POP_VC_WARNING()
#define EA_DISABLE_CLANG_WARNING( x )
#define EA_RESTORE_CLANG_WARNING()
#define EA_DISABLE_GCC_WARNING( x )
#define EA_RESTORE_GCC_WARNING()
#define EA_DEPRECATED
#define EA_REMOVE_AT_2025_OCT_MSG( msg )
#define EA_REMOVE_AT_2026_APRIL
#define EA_DEPRECATED_MESSAGE( msg )
#define EA_NON_COPYABLE( ClassName )                                                                                   \
	ClassName( const ClassName & ) = delete;                                                                           \
	ClassName &operator=( const ClassName & ) = delete
#ifndef EA_NODISCARD
#define EA_NODISCARD [[nodiscard]]
#endif
#define EA_COMPILER_NO_EXCEPTIONS 1
#define EA_COMPILER_NO_RTTI		  1
#define EA_COMPILER_NO_UNWIND	  1
#define EABASE_VERSION_N		  99999
#if defined( __has_builtin )
#define EA_COMPILER_HAS_BUILTIN( x ) __has_builtin( x )
#else
#define EA_COMPILER_HAS_BUILTIN( x ) 0
#endif
#if defined( __has_feature )
#define EA_COMPILER_HAS_FEATURE( x ) __has_feature( x )
#else
#define EA_COMPILER_HAS_FEATURE( x ) 0
#endif
#define EA_IS_ENABLED( x )			   0
#define EA_HAVE_CPP11_INITIALIZER_LIST 1
#define EA_HAVE_CPP11_TUPLES		   1
#define EA_HAVE_CPP11_TYPE_TRAITS	   1
#define EA_HAVE_CPP11_ATOMIC		   1

#endif // TERRAIN3D_EASTL_CONFIG_H
