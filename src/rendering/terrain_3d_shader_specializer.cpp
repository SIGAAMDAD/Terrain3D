// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "rendering/terrain_3d_shader_specializer.h"

#include "Terrain3DConstants.h"
#include "core/terrain_3d_compiler.h"

#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/templates/vector.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>

using namespace godot;

typedef enum t3dConditionalOwnership_e {
	T3D_CONDITIONAL_PRESERVED,
	T3D_CONDITIONAL_SPECIALIZED,
} t3dConditionalOwnership_t;

typedef struct t3dConditionalFrame_s {
	t3dConditionalOwnership_t m_ownership = T3D_CONDITIONAL_PRESERVED;
	bool m_bParentEmit = true;
	bool m_bBranchTaken = false;
} t3dConditionalFrame_t;

class Terrain3DStaticExpressionParser {
	const String &m_expression;
	const t3dShaderStaticState_t &m_state;
	int m_index = 0;
	String m_error;

	void skipWhitespace( void ) {
		while ( m_index < m_expression.length() ) {
			const char32_t c = m_expression[m_index];
			if ( c != ' ' && c != '\t' && c != '\r' ) {
				break;
			}
			++m_index;
		}
	}

	bool consume( const String &rToken ) {
		skipWhitespace();
		if ( m_expression.substr( m_index, rToken.length() ) != rToken ) {
			return false;
		}
		m_index += rToken.length();
		return true;
	}

	bool getMacroValue( const String &rName, int64_t &nValue ) {
		if ( rName == "T3D_TERRAIN_SAMPLING_MODE" ) {
			nValue = m_state.m_nTerrainSamplingMode;
		} else if ( rName == "T3D_TERRAIN_NORMAL_MODE" ) {
			nValue = m_state.m_nTerrainNormalMode;
		} else if ( rName == "T3D_COLOR_SAMPLING_MODE" ) {
			nValue = m_state.m_nColorSamplingMode;
		} else if ( rName == "T3D_PROJECTION_MODE" ) {
			nValue = m_state.m_nProjectionMode;
		} else if ( rName == "T3D_TEXTURE_SURFACE_MODE" ) {
			nValue = m_state.m_nTextureSurfaceMode;
		} else if ( rName == "T3D_SAMPLE_SURFACE_DATA" ) {
			nValue = m_state.m_bSampleSurfaceData ? 1 : 0;
		} else if ( rName == "T3D_OUTPUT_ROUGHNESS" ) {
			nValue = m_state.m_bOutputRoughness ? 1 : 0;
		} else if ( rName == "T3D_OUTPUT_SPECULAR" ) {
			nValue = m_state.m_bOutputSpecular ? 1 : 0;
		} else if ( rName == "T3D_OUTPUT_NORMAL_MAP" ) {
			nValue = m_state.m_bOutputNormalMap ? 1 : 0;
		} else if ( rName == "T3D_OUTPUT_AO" ) {
			nValue = m_state.m_bOutputAo ? 1 : 0;
		} else if ( rName == "T3D_SHADING_MODE" ) {
			nValue = m_state.m_nTerrainShadingMode;
		} else if ( rName == "T3D_DETILING_MODE" ) {
			nValue = m_state.m_nDetilingMode;
		} else if ( rName == "T3D_WORLD_NOISE_SHADER_MODE" ) {
			nValue = m_state.m_nWorldNoiseShaderMode;
		} else if ( rName == "T3D_FAST_BLEND_CURVE" ) {
			nValue = m_state.m_bFastBlendCurve ? 1 : 0;
		} else if ( rName == "T3D_CONTROL_MAPS_IMPLICIT" ) {
			nValue = m_state.m_bControlMapsImplicit ? 1 : 0;
		} else if ( rName == "T3D_COLOR_MAPS_IMPLICIT" ) {
			nValue = m_state.m_bColorMapsImplicit ? 1 : 0;
		} else if ( rName == "T3D_AUTO_SHADER_ENABLED" ) {
			nValue = m_state.m_bAutoShaderEnabled ? 1 : 0;
		} else {
			m_error = String( "Unknown Terrain3D shader macro: " ) + rName;
			return false;
		}
		return true;
	}

	bool parseIdentifierOrInteger( int64_t &nValue ) {
		skipWhitespace();
		if ( m_index >= m_expression.length() ) {
			m_error = "Unexpected end of Terrain3D shader expression";
			return false;
		}
		const int start = m_index;
		if ( m_expression[m_index] == '-' || ( m_expression[m_index] >= '0' && m_expression[m_index] <= '9' ) ) {
			if ( m_expression[m_index] == '-' ) {
				++m_index;
			}
			const int digits = m_index;
			while ( m_index < m_expression.length() && m_expression[m_index] >= '0' && m_expression[m_index] <= '9' ) {
				++m_index;
			}
			if ( digits == m_index ) {
				m_error = "Invalid integer in Terrain3D shader expression";
				return false;
			}
			nValue = m_expression.substr( start, m_index - start ).to_int();
			return true;
		}

		while ( m_index < m_expression.length() ) {
			const char32_t c = m_expression[m_index];
			const bool identifier_char =
					( c >= 'A' && c <= 'Z' ) || ( c >= 'a' && c <= 'z' ) || ( c >= '0' && c <= '9' ) || c == '_';
			if ( !identifier_char ) {
				break;
			}
			++m_index;
		}
		if ( start == m_index ) {
			m_error = "Expected macro or integer in Terrain3D shader expression";
			return false;
		}
		return getMacroValue( m_expression.substr( start, m_index - start ), nValue );
	}

	bool parseValue( int64_t &nValue ) {
		skipWhitespace();
		if ( consume( "(" ) ) {
			bool bNested = false;
			if ( !parseOr( bNested ) || !consume( ")" ) ) {
				if ( m_error.is_empty() ) {
					m_error = "Unbalanced parentheses in Terrain3D shader expression";
				}
				return false;
			}
			nValue = bNested ? 1 : 0;
			return true;
		}
		return parseIdentifierOrInteger( nValue );
	}

	bool parseComparison( bool &bValue ) {
		int64_t lhs = 0;
		if ( !parseValue( lhs ) ) {
			return false;
		}
		if ( consume( "==" ) ) {
			int64_t rhs = 0;
			if ( !parseValue( rhs ) ) {
				return false;
			}
			bValue = lhs == rhs;
			return true;
		}
		if ( consume( "!=" ) ) {
			int64_t rhs = 0;
			if ( !parseValue( rhs ) ) {
				return false;
			}
			bValue = lhs != rhs;
			return true;
		}
		bValue = lhs != 0;
		return true;
	}

	bool parseUnary( bool &bValue ) {
		if ( consume( "!" ) ) {
			if ( !parseUnary( bValue ) ) {
				return false;
			}
			bValue = !bValue;
			return true;
		}
		return parseComparison( bValue );
	}

	bool parseAnd( bool &bValue ) {
		if ( !parseUnary( bValue ) ) {
			return false;
		}
		while ( consume( "&&" ) ) {
			bool rhs = false;
			if ( !parseUnary( rhs ) ) {
				return false;
			}
			bValue = bValue && rhs;
		}
		return true;
	}

	bool parseOr( bool &bValue ) {
		if ( !parseAnd( bValue ) ) {
			return false;
		}
		while ( consume( "||" ) ) {
			bool rhs = false;
			if ( !parseAnd( rhs ) ) {
				return false;
			}
			bValue = bValue || rhs;
		}
		return true;
	}

public:
	EA_FORCE_INLINE Terrain3DStaticExpressionParser( const String &rExpression, const t3dShaderStaticState_t &rState ) :
			m_expression( rExpression ), m_state( rState ) {}

	bool evaluate( bool &bValue, String &rFailureSummary ) {
		if ( !parseOr( bValue ) ) {
			rFailureSummary = m_error;
			return false;
		}
		skipWhitespace();
		if ( m_index != m_expression.length() ) {
			rFailureSummary =
					String( "Unsupported trailing syntax in Terrain3D shader expression: " ) + m_expression.substr( m_index );
			return false;
		}
		return true;
	}
};

static bool t3dShaderSpecializerContainsEnabledFeature( const PackedStringArray &rFeatures, const String &rFeature ) {
	for ( int index = 0; index < rFeatures.size(); ++index ) {
		if ( rFeatures[index] == rFeature ) {
			return true;
		}
	}
	return false;
}

EA_FORCE_INLINE static bool t3dShaderSpecializerIsTerrain3dFeatureMacro( const String &rName ) {
	return rName.begins_with( "T3D_ENABLE_" );
}

EA_FORCE_INLINE static bool t3dShaderSpecializerIsTerrain3dStaticExpression( const String &rExpression ) {
	return rExpression.find( "T3D_" ) >= 0;
}

static String t3dShaderSpecializerGetShaderDirectiveCode( const String &rLine ) {
	String directive = rLine.strip_edges();
	if ( !directive.begins_with( "#" ) ) {
		return directive;
	}
	const int comment = directive.find( " //" );
	if ( comment >= 0 ) {
		directive = directive.substr( 0, comment ).strip_edges();
	}
	return directive;
}

static bool t3dShaderSpecializerIsPathInStack( const Vector<String> &rStack, const String &rPath ) {
	for ( int index = 0; index < rStack.size(); ++index ) {
		if ( rStack[index] == rPath ) {
			return true;
		}
	}
	return false;
}

static bool t3dShaderSpecializerExpandShaderSourceRecursive( const String &rPath,
		Vector<String> &rStack,
		String &rOutputSource,
		String &rFailureSummary ) {
	if ( rStack.size() >= s_nShaderMaxConditionalDepth ) {
		rFailureSummary = String( "Terrain3D shader include depth exceeded while expanding: " ) + rPath;
		return false;
	}
	if ( t3dShaderSpecializerIsPathInStack( rStack, rPath ) ) {
		rFailureSummary = String( "Terrain3D shader include cycle detected at: " ) + rPath;
		return false;
	}

	const Ref<FileAccess> file = FileAccess::open( rPath, FileAccess::READ );
	if ( file.is_null() ) {
		rFailureSummary = String( "Failed to open Terrain3D shader source: " ) + rPath;
		return false;
	}
	const String source = file->get_as_text();
	if ( source.is_empty() ) {
		rFailureSummary = String( "Terrain3D shader source is empty: " ) + rPath;
		return false;
	}

	rStack.push_back( rPath );
	const PackedStringArray lines = source.split( "\n", true );
	PackedStringArray output;
	for ( int line_index = 0; line_index < lines.size(); ++line_index ) {
		const String &line = lines[line_index];
		const String directive = t3dShaderSpecializerGetShaderDirectiveCode( line );
		if ( !directive.begins_with( "#include" ) ) {
			output.push_back( line );
			continue;
		}

		const int quote_start = directive.find( "\"" );
		const int quote_end = quote_start >= 0 ? directive.find( "\"", quote_start + 1 ) : -1;
		if ( quote_start < 0 || quote_end <= quote_start + 1 ) {
			rStack.remove_at( rStack.size() - 1 );
			rFailureSummary = String( "Malformed Terrain3D shader include in " ) + rPath + String( ": " ) + directive;
			return false;
		}
		String include_path = directive.substr( quote_start + 1, quote_end - quote_start - 1 );
		if ( !include_path.begins_with( "res://" ) ) {
			include_path = rPath.get_base_dir().path_join( include_path ).simplify_path();
		}
		String expanded_include;
		if ( !t3dShaderSpecializerExpandShaderSourceRecursive(
					 include_path, rStack, expanded_include, rFailureSummary ) ) {
			rStack.remove_at( rStack.size() - 1 );
			return false;
		}
		output.push_back( expanded_include );
	}
	rStack.remove_at( rStack.size() - 1 );

	rOutputSource = String( "\n" ).join( output );
	if ( source.ends_with( "\n" ) ) {
		rOutputSource += "\n";
	}
	return true;
}

bool t3dExpandShaderIncludes( const String &rRootPath, String &rOutputSource, String &rFailureSummary ) {
	Vector<String> stack;
	rOutputSource = String();
	rFailureSummary = String();
	return t3dShaderSpecializerExpandShaderSourceRecursive( rRootPath, stack, rOutputSource, rFailureSummary );
}

bool t3dValidateAssembledShaderSource( const String &rSource, String &rFailureSummary ) {
	const PackedStringArray lines = rSource.split( "\n", true );
	for ( int line_index = 0; line_index < lines.size(); ++line_index ) {
		const String directive = t3dShaderSpecializerGetShaderDirectiveCode( lines[line_index] );
		if ( directive.begins_with( "#include" ) ) {
			rFailureSummary =
					String( "Unexpanded shader include remains at assembled line " ) + String::num_int64( line_index + 1 );
			return false;
		}
		if ( directive.begins_with( "#if " ) || directive.begins_with( "#ifdef " ) ||
				directive.begins_with( "#ifndef " ) || directive.begins_with( "#elif " ) || directive == "#else" ||
				directive == "#endif" ) {
			rFailureSummary = String( "Conditional shader directive escaped Terrain3D specialization at assembled line " ) +
					String::num_int64( line_index + 1 ) + String( ": " ) + directive;
			return false;
		}
		if ( directive.find( "T3D_STANDALONE_DEFAULTS" ) >= 0 || directive.find( "T3D_TEMPLATE_CONTEXT" ) >= 0 ||
				( directive.begins_with( "#" ) && directive.find( "T3D_" ) >= 0 ) ) {
			rFailureSummary = String( "Unresolved Terrain3D preprocessing state remains at assembled line " ) +
					String::num_int64( line_index + 1 ) + String( ": " ) + directive;
			return false;
		}
	}
	return true;
}

bool t3dAssembleShaderSource( const String &rExpandedSource,
		const t3dShaderStaticState_t &rState,
		const PackedStringArray &rEnabledFeatures,
		String &rOutputSource,
		String &rFailureSummary ) {
	const PackedStringArray lines = rExpandedSource.split( "\n", true );
	PackedStringArray output;
	t3dConditionalFrame_t stack[s_nShaderMaxConditionalDepth] = {};
	int depth = 0;
	bool bEmit = true;
	bool bSkipStandaloneDefaults = false;

	for ( int line_index = 0; line_index < lines.size(); ++line_index ) {
		const String &line = lines[line_index];
		const String directive = t3dShaderSpecializerGetShaderDirectiveCode( line );
		if ( directive == "// T3D_STANDALONE_DEFAULTS_BEGIN" ) {
			if ( bSkipStandaloneDefaults ) {
				rFailureSummary =
						String( "Nested Terrain3D standalone-default block at line " ) + String::num_int64( line_index + 1 );
				return false;
			}
			bSkipStandaloneDefaults = true;
			continue;
		}
		if ( directive == "// T3D_STANDALONE_DEFAULTS_END" ) {
			if ( !bSkipStandaloneDefaults ) {
				rFailureSummary = String( "Unmatched Terrain3D standalone-default terminator at line " ) +
						String::num_int64( line_index + 1 );
				return false;
			}
			bSkipStandaloneDefaults = false;
			continue;
		}
		if ( bSkipStandaloneDefaults ) {
			continue;
		}

		const bool bIfdef = directive.begins_with( "#ifdef " );
		const bool bIfndef = directive.begins_with( "#ifndef " );
		if ( bIfdef || bIfndef ) {
			if ( depth >= s_nShaderMaxConditionalDepth ) {
				rFailureSummary =
						String( "Terrain3D shader conditional depth exceeded at line " ) + String::num_int64( line_index + 1 );
				return false;
			}
			const String macro = directive.substr( bIfdef ? 7 : 8 ).strip_edges();
			bool bOwned = false;
			bool bValue = false;
			if ( macro == "T3D_TEMPLATE_CONTEXT" ) {
				bOwned = true;
				bValue = true;
			} else if ( t3dShaderSpecializerIsTerrain3dFeatureMacro( macro ) ) {
				bOwned = true;
				const String feature = macro.substr( String( "T3D_ENABLE_" ).length() );
				bValue = t3dShaderSpecializerContainsEnabledFeature( rEnabledFeatures, feature );
			}
			if ( bIfndef ) {
				bValue = !bValue;
			}

			t3dConditionalFrame_t &frame = stack[depth++];
			frame.m_bParentEmit = bEmit;
			frame.m_bBranchTaken = bOwned && bValue;
			frame.m_ownership = bOwned ? T3D_CONDITIONAL_SPECIALIZED : T3D_CONDITIONAL_PRESERVED;
			if ( bOwned ) {
				bEmit = bEmit && bValue;
			} else if ( bEmit ) {
				output.push_back( line );
			}
			continue;
		}

		if ( directive.begins_with( "#if " ) ) {
			if ( depth >= s_nShaderMaxConditionalDepth ) {
				rFailureSummary =
						String( "Terrain3D shader conditional depth exceeded at line " ) + String::num_int64( line_index + 1 );
				return false;
			}
			const String expression = directive.substr( 4 ).strip_edges();
			const bool bOwned = t3dShaderSpecializerIsTerrain3dStaticExpression( expression );
			bool bValue = false;
			if ( bOwned ) {
				Terrain3DStaticExpressionParser parser( expression, rState );
				if ( !parser.evaluate( bValue, rFailureSummary ) ) {
					rFailureSummary = String( "Terrain3D shader expression error at line " ) +
							String::num_int64( line_index + 1 ) + String( ": " ) + rFailureSummary + String( " [" ) +
							expression + String( "]" );
					return false;
				}
			}
			t3dConditionalFrame_t &frame = stack[depth++];
			frame.m_bParentEmit = bEmit;
			frame.m_bBranchTaken = bOwned && bValue;
			frame.m_ownership = bOwned ? T3D_CONDITIONAL_SPECIALIZED : T3D_CONDITIONAL_PRESERVED;
			if ( bOwned ) {
				bEmit = bEmit && bValue;
			} else if ( bEmit ) {
				output.push_back( line );
			}
			continue;
		}

		if ( directive.begins_with( "#elif " ) ) {
			if ( depth <= 0 ) {
				rFailureSummary = String( "Unmatched #elif at line " ) + String::num_int64( line_index + 1 );
				return false;
			}
			t3dConditionalFrame_t &frame = stack[depth - 1];
			if ( frame.m_ownership == T3D_CONDITIONAL_PRESERVED ) {
				if ( frame.m_bParentEmit ) {
					output.push_back( line );
				}
				bEmit = frame.m_bParentEmit;
				continue;
			}
			const String expression = directive.substr( 6 ).strip_edges();
			if ( !t3dShaderSpecializerIsTerrain3dStaticExpression( expression ) ) {
				rFailureSummary = String( "Terrain3D-owned conditional mixes with non-Terrain3D #elif at line " ) +
						String::num_int64( line_index + 1 );
				return false;
			}
			bool bValue = false;
			Terrain3DStaticExpressionParser parser( expression, rState );
			if ( !parser.evaluate( bValue, rFailureSummary ) ) {
				rFailureSummary = String( "Terrain3D shader #elif expression error at line " ) +
						String::num_int64( line_index + 1 ) + String( ": " ) + rFailureSummary + String( " [" ) +
						expression + String( "]" );
				return false;
			}
			const bool bSelect = !frame.m_bBranchTaken && bValue;
			frame.m_bBranchTaken = frame.m_bBranchTaken || bValue;
			bEmit = frame.m_bParentEmit && bSelect;
			continue;
		}

		if ( directive == "#else" ) {
			if ( depth <= 0 ) {
				rFailureSummary = String( "Unmatched #else at line " ) + String::num_int64( line_index + 1 );
				return false;
			}
			t3dConditionalFrame_t &frame = stack[depth - 1];
			if ( frame.m_ownership == T3D_CONDITIONAL_PRESERVED ) {
				if ( frame.m_bParentEmit ) {
					output.push_back( line );
				}
				bEmit = frame.m_bParentEmit;
				continue;
			}
			bEmit = frame.m_bParentEmit && !frame.m_bBranchTaken;
			frame.m_bBranchTaken = true;
			continue;
		}

		if ( directive == "#endif" ) {
			if ( depth <= 0 ) {
				rFailureSummary = String( "Unmatched #endif at line " ) + String::num_int64( line_index + 1 );
				return false;
			}
			const t3dConditionalFrame_t frame = stack[--depth];
			if ( frame.m_ownership == T3D_CONDITIONAL_PRESERVED && frame.m_bParentEmit ) {
				output.push_back( line );
			}
			bEmit = frame.m_bParentEmit;
			continue;
		}

		if ( bEmit ) {
			output.push_back( line );
		}
	}

	if ( depth != 0 ) {
		rFailureSummary = "Unbalanced shader conditional stack after Terrain3D assembly";
		return false;
	}
	if ( bSkipStandaloneDefaults ) {
		rFailureSummary = "Unterminated Terrain3D standalone-default block";
		return false;
	}

	String assembled = String( "\n" ).join( output );
	if ( rExpandedSource.ends_with( "\n" ) ) {
		assembled += "\n";
	}
	if ( !t3dValidateAssembledShaderSource( assembled, rFailureSummary ) ) {
		return false;
	}
	rOutputSource = assembled;
	return true;
}
