// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "core/terrain_3d_compiler.h"
#include "core/unit_testing.h"
#include "core/terrain_3d_util.h"

namespace {
template <typename TLeft, typename TRight> EA_FORCE_INLINE void terrain3dLogDiffers( const TLeft &rA, const TRight &rB, const String &rDescription, const bool bExpected ) { const bool bDiffersResult = differs( rA, rB ); const String result = bDiffersResult == bExpected ? "PASSED" : "FAILED"; UtilityFunctions::print( rDescription, ": differs(", rA, ", ", rB, ") = ", bDiffersResult, " - ", result ); }
}

void testDiffers( void ) {
	UtilityFunctions::print( "=== Testing differs function ===" );

	// 1. Scalars: int, float (should use ==, differs if values differ)
	{
		int i1 = 42;
		int i2 = 42; // Same
		int i3 = 43; // Diff
		terrain3dLogDiffers( i1, i2, "int same", false );
		terrain3dLogDiffers( i1, i3, "int diff", true );

		double fF1 = 3.14;
		double fF2 = 3.14;	 // Same
		double fF3 = 3.14159; // Diff
		terrain3dLogDiffers( fF1, fF2, "float same", false );
		terrain3dLogDiffers( fF1, fF3, "float diff", true );
	}

	// 2. Vectors: Vector2, Vector2i, Vector3, Vector3i (should use ==)
	{
		Vector2 v21( 1.0, 2.0 );
		Vector2 v22( 1.0, 2.0 ); // Same
		Vector2 v23( 1.0, 3.0 ); // Diff
		terrain3dLogDiffers( v21, v22, "Vector2 same", false );
		terrain3dLogDiffers( v21, v23, "Vector2 diff", true );

		Vector2i v2i1( 1, 2 );
		Vector2i v2i2( 1, 2 ); // Same
		Vector2i v2i3( 1, 3 ); // Diff
		terrain3dLogDiffers( v2i1, v2i2, "Vector2i same", false );
		terrain3dLogDiffers( v2i1, v2i3, "Vector2i diff", true );

		Vector3 v31( 1.0, 2.0, 3.0 );
		Vector3 v32( 1.0, 2.0, 3.0 ); // Same
		Vector3 v33( 1.0, 2.0, 4.0 ); // Diff
		terrain3dLogDiffers( v31, v32, "Vector3 same", false );
		terrain3dLogDiffers( v31, v33, "Vector3 diff", true );

		Vector3i v3i1( 1, 2, 3 );
		Vector3i v3i2( 1, 2, 3 ); // Same
		Vector3i v3i3( 1, 2, 4 ); // Diff
		terrain3dLogDiffers( v3i1, v3i2, "Vector3i same", false );
		terrain3dLogDiffers( v3i1, v3i3, "Vector3i diff", true );
	}

	// 3. String: Share (COW), same value diff ptr, diff value
	{
		String s1 = "test";
		String s2 = s1;	   // Shared (COW)
		String s3( "test" ); // Separate alloc, same value
		String s4 = "diff";
		terrain3dLogDiffers( String(), String(), "String() vs String()", false );
		terrain3dLogDiffers( String(), String( "a" ), "String() vs String('a')", true );
		terrain3dLogDiffers( s1, s2, "String shared", false );
		terrain3dLogDiffers( s1, s3, "String same value diff ptr", false ); // Length match + == true
		terrain3dLogDiffers( s1, s4, "String diff value", true );			  // Length may match, but == false
	}

	// 4. StringName: Similar to String (uses ==, but test sharing if applicable)
	{
		StringName sn1 = "test";
		StringName sn2 = sn1;	// Shared
		StringName sn3( "test" ); // Separate, same value
		StringName sn4 = "diff";
		terrain3dLogDiffers( sn1, sn2, "StringName shared", false );
		terrain3dLogDiffers( sn1, sn3, "StringName same value diff ptr", false ); // == handles
		terrain3dLogDiffers( sn1, sn4, "StringName diff value", true );
	}

	// 5. Array: Share vs mutate
	{
		Array arr1;
		arr1.push_back( 42 );
		Array arr2 = arr1;								// Shared
		Array arr3;										// Empty diff
		arr3.push_back( 42 );								// Same content, but separate (differs=true, conservative)
		Array empty_arr;								// Explicit empty for size diff
		terrain3dLogDiffers( arr1, arr2, "Array shared", false ); // Same self ptr
		terrain3dLogDiffers( arr1, arr3, "Array same content diff ptr", true ); // Diff self ptr (no fallback for Array)
		terrain3dLogDiffers( arr1, empty_arr, "Array size diff", true );		  // Size mismatch
	}

	// 6. TypedArray: e.g., TypedArray<int>
	{
		TypedArray<int> ta1;
		ta1.push_back( 42 );
		TypedArray<int> ta2 = ta1;										 // Shared
		TypedArray<int> ta3;											 // Empty
		ta3.push_back( 42 );												 // Separate
		TypedArray<int> empty_ta;										 // Explicit empty
		terrain3dLogDiffers( ta1, ta2, "TypedArray shared", false );				 // Via Array base
		terrain3dLogDiffers( ta1, ta3, "TypedArray same content diff ptr", true ); // Diff self
		terrain3dLogDiffers( ta1, empty_ta, "TypedArray size diff", true );		 // Size mismatch
	}

	// 7. Dictionary: Share vs mutate
	{
		Dictionary dict1;
		dict1["key"] = 42;
		Dictionary dict2 = dict1; // Shared
		Dictionary dict3;		  // Empty
		dict3["key"] = 42;		  // Separate
		Dictionary empty_dict;	  // Explicit empty
		terrain3dLogDiffers( dict1, dict2, "Dictionary shared", false );
		terrain3dLogDiffers( dict1, dict3, "Dictionary same content diff ptr", true ); // Diff self
		terrain3dLogDiffers( dict1, empty_dict, "Dictionary size diff", true );		 // Size mismatch
	}

	// 8. Variant types: Wrap above (falls to ==)
	{
		Variant v_int1 = 42;
		Variant v_int2 = 42; // Same
		Variant v_int3 = 43; // Diff
		terrain3dLogDiffers( v_int1, v_int2, "Variant int same", false );
		terrain3dLogDiffers( v_int1, v_int3, "Variant int diff", true );

		Variant v_float1 = 3.14;
		Variant v_float2 = 3.14;
		Variant v_float3 = 3.14159;
		terrain3dLogDiffers( v_float1, v_float2, "Variant float same", false );
		terrain3dLogDiffers( v_float1, v_float3, "Variant float diff", true );

		Variant v_str1 = String( "test" );
		Variant v_str2 = String( "test" );
		Variant v_str3 = String( "diff" );
		terrain3dLogDiffers( v_str1, v_str2, "Variant String same", false );
		terrain3dLogDiffers( v_str1, v_str3, "Variant String diff", true );

		// Variant Object (use RefCounted for ref support)
		Ref<RefCounted> pRc1 = memnew( RefCounted );
		Ref<RefCounted> pRc2 = pRc1;				  // Same ref
		Ref<RefCounted> pRc3 = memnew( RefCounted ); // Diff
		Variant v_rc1 = pRc1;
		Variant v_rc2 = pRc2;
		Variant v_rc3 = pRc3;
		terrain3dLogDiffers( v_rc1, v_rc2, "Variant RefCounted same ref", false ); // Ref == true
		terrain3dLogDiffers( v_rc1, v_rc3, "Variant RefCounted diff ref", true );

		// Variant Array
		Array arr_var1;
		arr_var1.push_back( 42 );
		Variant v_arr1 = arr_var1;
		Array arr_var2 = arr_var1;
		Variant v_arr2 = arr_var2;
		terrain3dLogDiffers( v_arr1, v_arr2, "Variant Array shared", false ); // Variant == checks inner

		// Variant Dictionary
		Dictionary dict_var1;
		dict_var1["key"] = 42;
		Variant v_dict1 = dict_var1;
		Dictionary dict_var2 = dict_var1;
		Variant v_dict2 = dict_var2;
		terrain3dLogDiffers( v_dict1, v_dict2, "Variant Dictionary shared", false );
	}

	UtilityFunctions::print( "=== End differs tests ===" );
}
