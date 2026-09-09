// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
#ifndef TERRAIN3D_SYNC_H
#define TERRAIN3D_SYNC_H

#include <chrono>
#include <condition_variable>
#include <stdint.h>
#include <string.h>
#include <mutex>
#include <thread>
#include <utility>

#if defined( _MSC_VER )
#include <intrin.h>
#endif

typedef enum t3dMemoryOrder_e {
	T3D_MEMORY_ORDER_RELAXED = 0,
	T3D_MEMORY_ORDER_CONSUME,
	T3D_MEMORY_ORDER_ACQUIRE,
	T3D_MEMORY_ORDER_RELEASE,
	T3D_MEMORY_ORDER_ACQ_REL,
	T3D_MEMORY_ORDER_SEQ_CST
} t3dMemoryOrder_t;

#if !defined( _MSC_VER )
static inline int t3dNativeMemoryOrder( const t3dMemoryOrder_t eOrder ) {
	switch ( eOrder ) {
		case T3D_MEMORY_ORDER_RELAXED:
			return __ATOMIC_RELAXED;
		case T3D_MEMORY_ORDER_CONSUME:
			return __ATOMIC_CONSUME;
		case T3D_MEMORY_ORDER_ACQUIRE:
			return __ATOMIC_ACQUIRE;
		case T3D_MEMORY_ORDER_RELEASE:
			return __ATOMIC_RELEASE;
		case T3D_MEMORY_ORDER_ACQ_REL:
			return __ATOMIC_ACQ_REL;
		default:
			return __ATOMIC_SEQ_CST;
	}
}
#endif

#if defined( _MSC_VER )
template <typename T>
static inline T t3dAtomicMsLoad( const volatile T *pValue ) noexcept {
	static_assert( sizeof( T ) == 1u || sizeof( T ) == 2u || sizeof( T ) == 4u || sizeof( T ) == 8u, "Unsupported atomic width" );
	T value;
	if constexpr ( sizeof( T ) == 1u ) {
		const uint8_t nBits = ( uint8_t )_InterlockedCompareExchange8( ( volatile char * )pValue, 0, 0 );
		memcpy( &value, &nBits, sizeof( value ) );
	} else if constexpr ( sizeof( T ) == 2u ) {
		const uint16_t nBits = ( uint16_t )_InterlockedCompareExchange16( ( volatile short * )pValue, 0, 0 );
		memcpy( &value, &nBits, sizeof( value ) );
	} else if constexpr ( sizeof( T ) == 4u ) {
		const uint32_t nBits = ( uint32_t )_InterlockedCompareExchange( ( volatile long * )pValue, 0, 0 );
		memcpy( &value, &nBits, sizeof( value ) );
	} else {
		const uint64_t nBits = ( uint64_t )_InterlockedCompareExchange64( ( volatile __int64 * )pValue, 0, 0 );
		memcpy( &value, &nBits, sizeof( value ) );
	}
	return value;
}

template <typename T>
static inline T t3dAtomicMsExchange( volatile T *pValue, const T value ) noexcept {
	T previous;
	if constexpr ( sizeof( T ) == 1u ) {
		uint8_t nValueBits;
		memcpy( &nValueBits, &value, sizeof( nValueBits ) );
		const uint8_t nPreviousBits = ( uint8_t )_InterlockedExchange8( ( volatile char * )pValue, ( char )nValueBits );
		memcpy( &previous, &nPreviousBits, sizeof( previous ) );
	} else if constexpr ( sizeof( T ) == 2u ) {
		uint16_t nValueBits;
		memcpy( &nValueBits, &value, sizeof( nValueBits ) );
		const uint16_t nPreviousBits = ( uint16_t )_InterlockedExchange16( ( volatile short * )pValue, ( short )nValueBits );
		memcpy( &previous, &nPreviousBits, sizeof( previous ) );
	} else if constexpr ( sizeof( T ) == 4u ) {
		uint32_t nValueBits;
		memcpy( &nValueBits, &value, sizeof( nValueBits ) );
		const uint32_t nPreviousBits = ( uint32_t )_InterlockedExchange( ( volatile long * )pValue, ( long )nValueBits );
		memcpy( &previous, &nPreviousBits, sizeof( previous ) );
	} else {
		uint64_t nValueBits;
		memcpy( &nValueBits, &value, sizeof( nValueBits ) );
		const uint64_t nPreviousBits = ( uint64_t )_InterlockedExchange64( ( volatile __int64 * )pValue, ( __int64 )nValueBits );
		memcpy( &previous, &nPreviousBits, sizeof( previous ) );
	}
	return previous;
}

template <typename T>
static inline bool t3dAtomicMsCompareExchange( volatile T *pValue, T &rExpected, const T desired ) noexcept {
	T observed;
	if constexpr ( sizeof( T ) == 1u ) {
		uint8_t nExpectedBits;
		uint8_t nDesiredBits;
		memcpy( &nExpectedBits, &rExpected, sizeof( nExpectedBits ) );
		memcpy( &nDesiredBits, &desired, sizeof( nDesiredBits ) );
		const uint8_t nObservedBits =
		        ( uint8_t )_InterlockedCompareExchange8( ( volatile char * )pValue, ( char )nDesiredBits, ( char )nExpectedBits );
		memcpy( &observed, &nObservedBits, sizeof( observed ) );
	} else if constexpr ( sizeof( T ) == 2u ) {
		uint16_t nExpectedBits;
		uint16_t nDesiredBits;
		memcpy( &nExpectedBits, &rExpected, sizeof( nExpectedBits ) );
		memcpy( &nDesiredBits, &desired, sizeof( nDesiredBits ) );
		const uint16_t nObservedBits = ( uint16_t )_InterlockedCompareExchange16(
		        ( volatile short * )pValue, ( short )nDesiredBits, ( short )nExpectedBits );
		memcpy( &observed, &nObservedBits, sizeof( observed ) );
	} else if constexpr ( sizeof( T ) == 4u ) {
		uint32_t nExpectedBits;
		uint32_t nDesiredBits;
		memcpy( &nExpectedBits, &rExpected, sizeof( nExpectedBits ) );
		memcpy( &nDesiredBits, &desired, sizeof( nDesiredBits ) );
		const uint32_t nObservedBits =
		        ( uint32_t )_InterlockedCompareExchange( ( volatile long * )pValue, ( long )nDesiredBits, ( long )nExpectedBits );
		memcpy( &observed, &nObservedBits, sizeof( observed ) );
	} else {
		uint64_t nExpectedBits;
		uint64_t nDesiredBits;
		memcpy( &nExpectedBits, &rExpected, sizeof( nExpectedBits ) );
		memcpy( &nDesiredBits, &desired, sizeof( nDesiredBits ) );
		const uint64_t nObservedBits = ( uint64_t )_InterlockedCompareExchange64(
		        ( volatile __int64 * )pValue, ( __int64 )nDesiredBits, ( __int64 )nExpectedBits );
		memcpy( &observed, &nObservedBits, sizeof( observed ) );
	}
	if ( memcmp( &observed, &rExpected, sizeof( T ) ) == 0 ) {
		return true;
	}
	rExpected = observed;
	return false;
}

template <typename T>
static inline T t3dAtomicMsFetchAdd( volatile T *pValue, const T value ) noexcept {
	if constexpr ( sizeof( T ) == 1u ) {
		return ( T )_InterlockedExchangeAdd8( ( volatile char * )pValue, ( char )value );
	} else if constexpr ( sizeof( T ) == 2u ) {
		return ( T )_InterlockedExchangeAdd16( ( volatile short * )pValue, ( short )value );
	} else if constexpr ( sizeof( T ) == 4u ) {
		return ( T )_InterlockedExchangeAdd( ( volatile long * )pValue, ( long )value );
	} else {
		return ( T )_InterlockedExchangeAdd64( ( volatile __int64 * )pValue, ( __int64 )value );
	}
}

template <typename T>
static inline T t3dAtomicMsFetchOr( volatile T *pValue, const T value ) noexcept {
	if constexpr ( sizeof( T ) == 1u ) {
		return ( T )_InterlockedOr8( ( volatile char * )pValue, ( char )value );
	} else if constexpr ( sizeof( T ) == 2u ) {
		return ( T )_InterlockedOr16( ( volatile short * )pValue, ( short )value );
	} else if constexpr ( sizeof( T ) == 4u ) {
		return ( T )_InterlockedOr( ( volatile long * )pValue, ( long )value );
	} else {
		return ( T )_InterlockedOr64( ( volatile __int64 * )pValue, ( __int64 )value );
	}
}

template <typename T>
static inline T t3dAtomicMsFetchAnd( volatile T *pValue, const T value ) noexcept {
	if constexpr ( sizeof( T ) == 1u ) {
		return ( T )_InterlockedAnd8( ( volatile char * )pValue, ( char )value );
	} else if constexpr ( sizeof( T ) == 2u ) {
		return ( T )_InterlockedAnd16( ( volatile short * )pValue, ( short )value );
	} else if constexpr ( sizeof( T ) == 4u ) {
		return ( T )_InterlockedAnd( ( volatile long * )pValue, ( long )value );
	} else {
		return ( T )_InterlockedAnd64( ( volatile __int64 * )pValue, ( __int64 )value );
	}
}
#endif

template <typename T>
class t3dAtomic_t {
	volatile T m_value;

public:
	t3dAtomic_t( void ) noexcept : m_value( ( T )0 ) {
	}
	t3dAtomic_t( const T value ) noexcept : m_value( value ) {
	}
	t3dAtomic_t( const t3dAtomic_t & ) = delete;
	t3dAtomic_t &operator=( const t3dAtomic_t & ) = delete;

	T load( const t3dMemoryOrder_t eOrder = T3D_MEMORY_ORDER_SEQ_CST ) const noexcept {
#if defined( _MSC_VER )
		( void )eOrder;
		return t3dAtomicMsLoad( &m_value );
#else
		return __atomic_load_n( &m_value, t3dNativeMemoryOrder( eOrder ) );
#endif
	}

	void store( const T value, const t3dMemoryOrder_t eOrder = T3D_MEMORY_ORDER_SEQ_CST ) noexcept {
#if defined( _MSC_VER )
		( void )eOrder;
		( void )t3dAtomicMsExchange( &m_value, value );
#else
		__atomic_store_n( &m_value, value, t3dNativeMemoryOrder( eOrder ) );
#endif
	}

	T exchange( const T value, const t3dMemoryOrder_t eOrder = T3D_MEMORY_ORDER_SEQ_CST ) noexcept {
#if defined( _MSC_VER )
		( void )eOrder;
		return t3dAtomicMsExchange( &m_value, value );
#else
		return __atomic_exchange_n( &m_value, value, t3dNativeMemoryOrder( eOrder ) );
#endif
	}

	bool compare_exchange_weak( T &rExpected, const T desired, const t3dMemoryOrder_t eSuccess, const t3dMemoryOrder_t eFailure ) noexcept {
#if defined( _MSC_VER )
		( void )eSuccess;
		( void )eFailure;
		return t3dAtomicMsCompareExchange( &m_value, rExpected, desired );
#else
		return __atomic_compare_exchange_n(
		        &m_value, &rExpected, desired, true, t3dNativeMemoryOrder( eSuccess ), t3dNativeMemoryOrder( eFailure ) );
#endif
	}

	bool compare_exchange_weak( T &rExpected, const T desired, const t3dMemoryOrder_t eOrder = T3D_MEMORY_ORDER_SEQ_CST ) noexcept {
		return compare_exchange_weak( rExpected, desired, eOrder, eOrder );
	}

	bool compare_exchange_strong(
	        T &rExpected, const T desired, const t3dMemoryOrder_t eSuccess, const t3dMemoryOrder_t eFailure ) noexcept {
#if defined( _MSC_VER )
		( void )eSuccess;
		( void )eFailure;
		return t3dAtomicMsCompareExchange( &m_value, rExpected, desired );
#else
		return __atomic_compare_exchange_n(
		        &m_value, &rExpected, desired, false, t3dNativeMemoryOrder( eSuccess ), t3dNativeMemoryOrder( eFailure ) );
#endif
	}

	bool compare_exchange_strong( T &rExpected, const T desired, const t3dMemoryOrder_t eOrder = T3D_MEMORY_ORDER_SEQ_CST ) noexcept {
		return compare_exchange_strong( rExpected, desired, eOrder, eOrder );
	}

	T fetch_add( const T value, const t3dMemoryOrder_t eOrder = T3D_MEMORY_ORDER_SEQ_CST ) noexcept {
#if defined( _MSC_VER )
		( void )eOrder;
		return t3dAtomicMsFetchAdd( &m_value, value );
#else
		return __atomic_fetch_add( &m_value, value, t3dNativeMemoryOrder( eOrder ) );
#endif
	}

	T fetch_sub( const T value, const t3dMemoryOrder_t eOrder = T3D_MEMORY_ORDER_SEQ_CST ) noexcept {
#if defined( _MSC_VER )
		const T delta = ( T )( ( T )0 - value );
		( void )eOrder;
		return t3dAtomicMsFetchAdd( &m_value, delta );
#else
		return __atomic_fetch_sub( &m_value, value, t3dNativeMemoryOrder( eOrder ) );
#endif
	}

	T fetch_or( const T value, const t3dMemoryOrder_t eOrder = T3D_MEMORY_ORDER_SEQ_CST ) noexcept {
#if defined( _MSC_VER )
		( void )eOrder;
		return t3dAtomicMsFetchOr( &m_value, value );
#else
		return __atomic_fetch_or( &m_value, value, t3dNativeMemoryOrder( eOrder ) );
#endif
	}

	T fetch_and( const T value, const t3dMemoryOrder_t eOrder = T3D_MEMORY_ORDER_SEQ_CST ) noexcept {
#if defined( _MSC_VER )
		( void )eOrder;
		return t3dAtomicMsFetchAnd( &m_value, value );
#else
		return __atomic_fetch_and( &m_value, value, t3dNativeMemoryOrder( eOrder ) );
#endif
	}

	bool is_lock_free( void ) const noexcept {
#if defined( _MSC_VER )
		return true;
#else
		return __atomic_is_lock_free( sizeof( T ), ( const volatile void * )&m_value );
#endif
	}

	T operator=( const T value ) noexcept {
		store( value );
		return value;
	}

	operator T( void ) const noexcept {
		return load();
	}

	T operator++( void ) noexcept {
		return fetch_add( ( T )1 ) + ( T )1;
	}
	T operator++( int ) noexcept {
		return fetch_add( ( T )1 );
	}
	T operator--( void ) noexcept {
		return fetch_sub( ( T )1 ) - ( T )1;
	}
	T operator--( int ) noexcept {
		return fetch_sub( ( T )1 );
	}
	T operator+=( const T value ) noexcept {
		return fetch_add( value ) + value;
	}
	T operator-=( const T value ) noexcept {
		return fetch_sub( value ) - value;
	}
};

template <>
class t3dAtomic_t<float> {
	t3dAtomic_t<uint32_t> m_nBits;

	static uint32_t toBits( const float fValue ) noexcept {
		uint32_t nBits;
		memcpy( &nBits, &fValue, sizeof( nBits ) );
		return nBits;
	}

	static float fromBits( const uint32_t nBits ) noexcept {
		float fValue;
		memcpy( &fValue, &nBits, sizeof( fValue ) );
		return fValue;
	}

public:
	t3dAtomic_t( void ) noexcept : m_nBits( 0u ) {
	}
	t3dAtomic_t( const float fValue ) noexcept : m_nBits( toBits( fValue ) ) {
	}
	t3dAtomic_t( const t3dAtomic_t & ) = delete;
	t3dAtomic_t &operator=( const t3dAtomic_t & ) = delete;

	float load( const t3dMemoryOrder_t eOrder = T3D_MEMORY_ORDER_SEQ_CST ) const noexcept {
		return fromBits( m_nBits.load( eOrder ) );
	}

	void store( const float fValue, const t3dMemoryOrder_t eOrder = T3D_MEMORY_ORDER_SEQ_CST ) noexcept {
		m_nBits.store( toBits( fValue ), eOrder );
	}

	float exchange( const float fValue, const t3dMemoryOrder_t eOrder = T3D_MEMORY_ORDER_SEQ_CST ) noexcept {
		return fromBits( m_nBits.exchange( toBits( fValue ), eOrder ) );
	}

	bool compare_exchange_weak(
	        float &rExpected, const float fDesired, const t3dMemoryOrder_t eSuccess, const t3dMemoryOrder_t eFailure ) noexcept {
		uint32_t nExpected = toBits( rExpected );
		const bool bExchanged = m_nBits.compare_exchange_weak( nExpected, toBits( fDesired ), eSuccess, eFailure );
		if ( !bExchanged ) {
			rExpected = fromBits( nExpected );
		}
		return bExchanged;
	}

	bool compare_exchange_strong(
	        float &rExpected, const float fDesired, const t3dMemoryOrder_t eSuccess, const t3dMemoryOrder_t eFailure ) noexcept {
		uint32_t nExpected = toBits( rExpected );
		const bool bExchanged = m_nBits.compare_exchange_strong( nExpected, toBits( fDesired ), eSuccess, eFailure );
		if ( !bExchanged ) {
			rExpected = fromBits( nExpected );
		}
		return bExchanged;
	}

	bool is_lock_free( void ) const noexcept {
		return m_nBits.is_lock_free();
	}

	float operator=( const float fValue ) noexcept {
		store( fValue );
		return fValue;
	}

	operator float( void ) const noexcept {
		return load();
	}
};

typedef t3dAtomic_t<bool> t3dAtomicBool_t;
typedef t3dAtomic_t<uint8_t> t3dAtomicU8_t;
typedef t3dAtomic_t<uint16_t> t3dAtomicU16_t;
typedef t3dAtomic_t<uint32_t> t3dAtomicU32_t;
typedef t3dAtomic_t<uint64_t> t3dAtomicU64_t;

class t3dMutex_t {
	friend class t3dUniqueLock_t;
	std::mutex m_mutex;

public:
	t3dMutex_t( void ) = default;
	t3dMutex_t( const t3dMutex_t & ) = delete;
	t3dMutex_t &operator=( const t3dMutex_t & ) = delete;

	void lock( void ) {
		m_mutex.lock();
	}
	void unlock( void ) {
		m_mutex.unlock();
	}
	bool try_lock( void ) {
		return m_mutex.try_lock();
	}
};

class t3dScopedLock_t {
	t3dMutex_t *m_pMutex;

public:
	explicit t3dScopedLock_t( t3dMutex_t &rMutex ) : m_pMutex( &rMutex ) {
		m_pMutex->lock();
	}
	~t3dScopedLock_t( void ) {
		m_pMutex->unlock();
	}
	t3dScopedLock_t( const t3dScopedLock_t & ) = delete;
	t3dScopedLock_t &operator=( const t3dScopedLock_t & ) = delete;
};

class t3dUniqueLock_t {
	friend class t3dCondition_t;
	std::unique_lock<std::mutex> m_lock;

public:
	explicit t3dUniqueLock_t( t3dMutex_t &rMutex ) : m_lock( rMutex.m_mutex ) {
	}
	t3dUniqueLock_t( const t3dUniqueLock_t & ) = delete;
	t3dUniqueLock_t &operator=( const t3dUniqueLock_t & ) = delete;
};

class t3dCondition_t {
	std::condition_variable m_condition;

public:
	void wait( t3dUniqueLock_t &rLock ) {
		m_condition.wait( rLock.m_lock );
	}
	void notifyOne( void ) {
		m_condition.notify_one();
	}
	void notifyAll( void ) {
		m_condition.notify_all();
	}
};

class t3dThread_t {
	std::thread m_thread;

public:
	t3dThread_t( void ) noexcept = default;
	t3dThread_t( t3dThread_t && ) noexcept = default;
	t3dThread_t &operator=( t3dThread_t && ) noexcept = default;
	t3dThread_t( const t3dThread_t & ) = delete;
	t3dThread_t &operator=( const t3dThread_t & ) = delete;

	template <typename Fn, typename... Args>
	explicit t3dThread_t( Fn &&fn, Args &&...args ) : m_thread( std::forward<Fn>( fn ), std::forward<Args>( args )... ) {
	}

	bool joinable( void ) const noexcept {
		return m_thread.joinable();
	}
	void join( void ) {
		m_thread.join();
	}
};

typedef std::once_flag t3dOnceFlag_t;

template <typename Fn>
static inline void t3dCallOnce( t3dOnceFlag_t &rFlag, Fn fn ) {
	std::call_once( rFlag, fn );
}

static inline void t3dThreadYield( void ) {
	std::this_thread::yield();
}

static inline uint64_t t3dSteadyClockUsec( void ) {
	return ( uint64_t )std::chrono::duration_cast<std::chrono::microseconds>( std::chrono::steady_clock::now().time_since_epoch() ).count();
}

#endif // TERRAIN3D_SYNC_H
