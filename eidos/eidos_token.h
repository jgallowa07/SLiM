//
//  eidos_token.h
//  Eidos
//
//  Created by Ben Haller on 7/27/15.
//  Copyright (c) 2015 Philipp Messer.  All rights reserved.
//	A product of the Messer Lab, http://messerlab.org/software/
//

//	This file is part of Eidos.
//
//	Eidos is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by
//	the Free Software Foundation, either version 3 of the License, or (at your option) any later version.
//
//	Eidos is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
//	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more details.
//
//	You should have received a copy of the GNU General Public License along with Eidos.  If not, see <http://www.gnu.org/licenses/>.


#ifndef __Eidos__eidos_token__
#define __Eidos__eidos_token__

#include <stdio.h>

#include <string>
#include <ostream>

#include "eidos_global.h"


// An enumeration for all token types, whether real or virtual
enum class EidosTokenType {
	kTokenNone = 0,
	kTokenEOF,
	kTokenWhitespace,
	
	kTokenSemicolon,	// ;		statement terminator
	kTokenColon,		// :		range operator, as in R
	kTokenComma,		// ,		presently used for separating function parameters only
	kTokenLBrace,		// {		block delimiter
	kTokenRBrace,		// }		block delimiter
	kTokenLParen,		// (		subexpression delimiter
	kTokenRParen,		// )		subexpression delimiter
	kTokenLBracket,		// [		subset operator
	kTokenRBracket,		// ]		subset operator
	kTokenDot,			// .		member operator
	kTokenPlus,			// +		addition operator
	kTokenMinus,		// -		subtraction operator (unary or binary)
	kTokenMod,			// %		modulo operator
	kTokenMult,			// *		multiplication operator
	kTokenExp,			// ^		exponentiation operator
	
	kTokenAnd,			// &		boolean AND
	kTokenOr,			// |		boolean OR
	
	kTokenDiv,			// /		division operator
	kTokenComment,		// //		comment
	kTokenAssign,		// =		assignment
	kTokenEq,			// ==		equality test
	kTokenLt,			// <		less than test
	kTokenLtEq,			// <=		less than or equals test
	kTokenGt,			// >		greater than test
	kTokenGtEq,			// >=		greater than or equals test
	kTokenNot,			// !		boolean NOT
	kTokenNotEq,		// !=		not equals test
	
	kTokenNumber,		//			there is a single numeric token type for both ints and floats, for now at least
	kTokenString,		//			string literals are bounded by double quotes only, and recognize some escapes
	kTokenIdentifier,	//			all valid identifiers that are not keywords or operators
	
	// ----- VIRTUAL TOKENS; THESE WILL HAVE A STRING OF "" AND A LENGTH OF 0
	
	kTokenInterpreterBlock,			// a block of statements executed as a unit in the interpreter
	
	// these virtual tokens are not used by Eidos itself; they are provided as a convenience for
	// Contexts that embed Eidos within larger script files in a Context-defined format
	
	kTokenContextFile,				// an Eidos-based input file containing zero or more Eidos blocks in a Context-defined format
	kTokenContextEidosBlock,		// an Eidos-based script block with additional tokens in a Context-defined format
	
	// ----- ALL TOKENS AFTER THIS POINT SHOULD BE KEYWORDS MATCHED BY kTokenIdentifier
	
	kFirstIdentifierLikeToken,
	kTokenIf,			// if		conditional
	kTokenElse,			// else		conditional
	kTokenDo,			// do		loop while condition true
	kTokenWhile,		// while	loop while condition true
	kTokenFor,			// for		loop over set
	kTokenIn,			// in		loop over set
	kTokenNext,			// next		loop jump to end
	kTokenBreak,		// break	loop jump to completion
	kTokenReturn,		// return	return a value from the enclosing block
};

std::ostream &operator<<(std::ostream &p_outstream, const EidosTokenType p_token_type);


// A class representing a single token read from a script string
class EidosToken
{
	//	This class has its copy constructor and assignment operator disabled, to prevent accidental copying.
	
public:
	
	const EidosTokenType token_type_;			// the type of the token; one of the enumeration above
	const std::string token_string_;		// extracted string object for the token
	const int token_start_;					// character position within script_string_
	const int token_end_;					// character position within script_string_
	
	EidosToken(const EidosToken&) = delete;					// no copying
	EidosToken& operator=(const EidosToken&) = delete;		// no copying
	EidosToken(void) = delete;									// no null construction
	EidosToken(EidosTokenType p_token_type, const std::string &p_token_string, int p_token_start, int p_token_end);
};

std::ostream &operator<<(std::ostream &p_outstream, const EidosToken &p_token);


#endif /* defined(__Eidos__eidos_token__) */




























































