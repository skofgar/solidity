#include <boost/algorithm/string/predicate.hpp>
#include <boost/regex.hpp>
#include <libsolidity/ast/ASTBoogieUtils.h>
#include <libsolidity/ast/AST.h>
#include <libsolidity/ast/Types.h>
#include <libsolidity/ast/TypeProvider.h>
#include <liblangutil/SourceLocation.h>
#include <liblangutil/Exceptions.h>
#include <liblangutil/Scanner.h>

using namespace std;
using namespace dev;
using namespace dev::solidity;
using namespace langutil;

namespace bg = boogie;

using namespace boogie;

namespace dev
{
namespace solidity
{
string const ASTBoogieUtils::SOLIDITY_BALANCE = "balance";
string const ASTBoogieUtils::SOLIDITY_TRANSFER = "transfer";
string const ASTBoogieUtils::BOOGIE_TRANSFER = "__transfer";
string const ASTBoogieUtils::SOLIDITY_SEND = "send";
string const ASTBoogieUtils::BOOGIE_SEND = "__send";
string const ASTBoogieUtils::SOLIDITY_CALL = "call";
string const ASTBoogieUtils::BOOGIE_CALL = "__call";
string const ASTBoogieUtils::SOLIDITY_SUPER = "super";

string const ASTBoogieUtils::SOLIDITY_SENDER = "sender";
string const ASTBoogieUtils::SOLIDITY_VALUE = "value";

string const ASTBoogieUtils::SOLIDITY_ASSERT = "assert";
string const ASTBoogieUtils::SOLIDITY_REQUIRE = "require";
string const ASTBoogieUtils::SOLIDITY_REVERT = "revert";

string const ASTBoogieUtils::SOLIDITY_THIS = "this";
string const ASTBoogieUtils::VERIFIER_SUM = "__verifier_sum";
string const ASTBoogieUtils::VERIFIER_OLD = "__verifier_old";
string const ASTBoogieUtils::BOOGIE_CONSTRUCTOR = "__constructor";
string const ASTBoogieUtils::BOOGIE_LENGTH = "#length";
string const ASTBoogieUtils::BOOGIE_SUM = "#sum";
string const ASTBoogieUtils::BOOGIE_INT_CONST_TYPE = "int_const";
string const ASTBoogieUtils::ERR_TYPE = "__ERROR_UNSUPPORTED_TYPE";
string const ASTBoogieUtils::SOLIDITY_NOW = "now";
string const ASTBoogieUtils::BOOGIE_NOW = "__now";
string const ASTBoogieUtils::SOLIDITY_NUMBER = "number";
string const ASTBoogieUtils::BOOGIE_BLOCKNO = "__block__number";
string const ASTBoogieUtils::VERIFIER_OVERFLOW = "__verifier_overflow";

string const ASTBoogieUtils::ERR_EXPR = "__ERROR";

string const ASTBoogieUtils::BOOGIE_STOR = "stor";
string const ASTBoogieUtils::BOOGIE_MEM = "mem";

string const ASTBoogieUtils::DOCTAG_CONTRACT_INVAR = "invariant";
string const ASTBoogieUtils::DOCTAG_CONTRACT_INVARS_INCLUDE = "{contractInvariants}";
string const ASTBoogieUtils::DOCTAG_LOOP_INVAR = "invariant";
string const ASTBoogieUtils::DOCTAG_PRECOND = "precondition";
string const ASTBoogieUtils::DOCTAG_POSTCOND = "postcondition";
string const ASTBoogieUtils::DOCTAG_MODIFIES = "modifies";
string const ASTBoogieUtils::DOCTAG_MODIFIES_ALL = DOCTAG_MODIFIES + " *";
string const ASTBoogieUtils::DOCTAG_MODIFIES_COND = " if ";

ProcDeclRef ASTBoogieUtils::createTransferProc(BoogieContext& context)
{
	// Parameters: this, msg.sender, msg.value, amount
	vector<Binding> transferParams{
		{context.boogieThis()->getRefTo(), context.boogieThis()->getType() },
		{context.boogieMsgSender()->getRefTo(), context.boogieMsgSender()->getType() },
		{context.boogieMsgValue()->getRefTo(), context.boogieMsgValue()->getType() },
		{Expr::id("amount"), context.intType(256) }
	};

	// Type to pass around
	TypePointer tp_uint256 = TypeProvider::integer(256, IntegerType::Modifier::Unsigned);

	// Body
	bg::Block::Ref transferImpl = bg::Block::block();
	Expr::Ref this_bal = Expr::arrsel(context.boogieBalance()->getRefTo(), context.boogieThis()->getRefTo());
	Expr::Ref sender_bal = Expr::arrsel(context.boogieBalance()->getRefTo(), context.boogieMsgSender()->getRefTo());
	Expr::Ref amount = Expr::id("amount");

	// Precondition: there is enough ether to transfer
	auto geqResult = encodeArithBinaryOp(context, nullptr, langutil::Token::GreaterThanOrEqual, sender_bal, amount, 256, false);
	transferImpl->addStmt(Stmt::assume(geqResult.expr));
	// balance[this] += amount
	if (context.encoding() == BoogieContext::Encoding::MOD)
	{
		transferImpl->addStmts({
			Stmt::assume(ASTBoogieUtils::getTCCforExpr(this_bal, tp_uint256)),
			Stmt::assume(ASTBoogieUtils::getTCCforExpr(amount, tp_uint256))
		});
	}
	auto addBalance = encodeArithBinaryOp(context, nullptr, Token::Add, this_bal, amount, 256, false);
	if (context.overflow())
	{
		transferImpl->addStmts({
			Stmt::comment("Implicit assumption that balances cannot overflow"),
			Stmt::assume(addBalance.cc)
		});
	}
	transferImpl->addStmt(Stmt::assign(
			context.boogieBalance()->getRefTo(),
			Expr::arrupd(context.boogieBalance()->getRefTo(), context.boogieThis()->getRefTo(), addBalance.expr)));
	// balance[msg.sender] -= amount
	if (context.encoding() == BoogieContext::Encoding::MOD)
	{
		transferImpl->addStmt(Stmt::assume(ASTBoogieUtils::getTCCforExpr(sender_bal, tp_uint256)));
		transferImpl->addStmt(Stmt::assume(ASTBoogieUtils::getTCCforExpr(amount, tp_uint256)));
	}
	auto subSenderBalance = encodeArithBinaryOp(context, nullptr, Token::Sub, sender_bal, amount, 256, false);
	if (context.overflow())
	{
		transferImpl->addStmt(Stmt::comment("Implicit assumption that balances cannot overflow"));
		transferImpl->addStmt(Stmt::assume(subSenderBalance.cc));
	}
	transferImpl->addStmt(Stmt::assign(
			context.boogieBalance()->getRefTo(),
			Expr::arrupd(context.boogieBalance()->getRefTo(), context.boogieMsgSender()->getRefTo(), subSenderBalance.expr)));
	transferImpl->addStmt(Stmt::comment("TODO: call fallback, exception handling"));

	ProcDeclRef transfer = Decl::procedure(BOOGIE_TRANSFER, transferParams, {}, {}, {transferImpl});

	transfer->addAttrs({
		Attr::attr("inline", 1),
		Attr::attr("message", "transfer")
	});
	return transfer;
}

ProcDeclRef ASTBoogieUtils::createCallProc(BoogieContext& context)
{

	// Parameters: this, msg.sender, msg.value
	vector<Binding> callParams {
		{context.boogieThis()->getRefTo(), context.boogieThis()->getType() },
		{context.boogieMsgSender()->getRefTo(), context.boogieMsgSender()->getType() },
		{context.boogieMsgValue()->getRefTo(), context.boogieMsgValue()->getType() }
	};

	// Type to pass around
	TypePointer tp_uint256 = TypeProvider::integer(256, IntegerType::Modifier::Unsigned);

	// Get the type of the call function
	auto callType = TypeProvider::address()->memberType("call");
	auto callFunctionType = dynamic_cast<FunctionType const*>(callType);

	solAssert(callFunctionType, "");
	solAssert(callFunctionType->returnParameterTypes().size() == 2, "");

	// Return value
	vector<Binding> callReturns{
		{Expr::id("__result"), context.toBoogieType(callFunctionType->returnParameterTypes()[0], nullptr)},
		{Expr::id("__calldata"), context.toBoogieType(callFunctionType->returnParameterTypes()[1], nullptr)}
	};

	// Body
	// Successful transfer
	bg::Block::Ref thenBlock = bg::Block::block();
	Expr::Ref this_bal = Expr::arrsel(context.boogieBalance()->getRefTo(), context.boogieThis()->getRefTo());
	Expr::Ref msg_val = context.boogieMsgValue()->getRefTo();
	Expr::Ref result = Expr::id("__result");

	// balance[this] += msg.value
	if (context.encoding() == BoogieContext::Encoding::MOD)
	{
		thenBlock->addStmts({
			Stmt::assume(ASTBoogieUtils::getTCCforExpr(this_bal, tp_uint256)),
			Stmt::assume(ASTBoogieUtils::getTCCforExpr(msg_val, tp_uint256))
		});
	}
	auto addBalance = encodeArithBinaryOp(context, nullptr, Token::Add, this_bal, msg_val, 256, false);
	if (context.overflow())
	{
		thenBlock->addStmts({
			Stmt::comment("Implicit assumption that balances cannot overflow"),
			Stmt::assume(addBalance.cc)
		});
	}
	thenBlock->addStmt(Stmt::assign(
			context.boogieBalance()->getRefTo(),
			Expr::arrupd(context.boogieBalance()->getRefTo(), context.boogieThis()->getRefTo(), addBalance.expr)));
	thenBlock->addStmt(Stmt::assign(result, Expr::lit(true)));
	// Unsuccessful transfer
	bg::Block::Ref elseBlock = bg::Block::block();
	elseBlock->addStmt(Stmt::assign(result, Expr::lit(false)));
	// Nondeterministic choice between success and failure
	bg::Block::Ref callBlock = bg::Block::block();
	callBlock->addStmt(Stmt::comment("TODO: call fallback"));
	callBlock->addStmt(Stmt::ifelse(Expr::id("*"), thenBlock, elseBlock));

	ProcDeclRef callProc = Decl::procedure(BOOGIE_CALL, callParams, callReturns, {}, {callBlock});
	callProc->addAttr(Attr::attr("inline", 1));
	callProc->addAttr(Attr::attr("message", "call"));
	return callProc;
}

ProcDeclRef ASTBoogieUtils::createSendProc(BoogieContext& context)
{
	Expr::Ref amount = Expr::id("amount");
	Expr::Ref result = Expr::id("__result");
	// Parameters: this, msg.sender, msg.value, amount
	vector<Binding> sendParams {
		{context.boogieThis()->getRefTo(), context.boogieThis()->getType()},
		{context.boogieMsgSender()->getRefTo(), context.boogieMsgSender()->getType()},
		{context.boogieMsgValue()->getRefTo(), context.boogieMsgValue()->getType()},
		{amount, context.intType(256)}
	};

	// Type to pass around
	TypePointer tp_uint256 = TypeProvider::integer(256, IntegerType::Modifier::Unsigned);

	// Return value
	vector<Binding> sendReturns{ {result, context.boolType()} };

	// Body
	// Successful transfer
	bg::Block::Ref thenBlock = bg::Block::block();
	Expr::Ref this_bal = Expr::arrsel(context.boogieBalance()->getRefTo(), context.boogieThis()->getRefTo());
	Expr::Ref sender_bal = Expr::arrsel(context.boogieBalance()->getRefTo(), context.boogieMsgSender()->getRefTo());

	// balance[this] += amount
	if (context.encoding() == BoogieContext::Encoding::MOD)
	{
		thenBlock->addStmts({
			Stmt::assume(ASTBoogieUtils::getTCCforExpr(this_bal, tp_uint256)),
			Stmt::assume(ASTBoogieUtils::getTCCforExpr(amount, tp_uint256))
		});
	}
	auto addBalance = encodeArithBinaryOp(context, nullptr, Token::Add, this_bal, amount, 256, false);
	if (context.overflow())
	{
		thenBlock->addStmts({
			Stmt::comment("Implicit assumption that balances cannot overflow"),
			Stmt::assume(addBalance.cc)
		});
	}
	thenBlock->addStmt(Stmt::assign(
			context.boogieBalance()->getRefTo(),
			Expr::arrupd(context.boogieBalance()->getRefTo(), context.boogieThis()->getRefTo(), addBalance.expr)));
	// balance[msg.sender] -= amount
	if (context.encoding() == BoogieContext::Encoding::MOD)
	{
		thenBlock->addStmts({
			Stmt::assume(ASTBoogieUtils::getTCCforExpr(sender_bal, tp_uint256)),
			Stmt::assume(ASTBoogieUtils::getTCCforExpr(amount, tp_uint256))
		});
	}
	auto subSenderBalance = encodeArithBinaryOp(context, nullptr, Token::Sub, sender_bal, amount, 256, false);
	if (context.overflow())
	{
		thenBlock->addStmts({
			Stmt::comment("Implicit assumption that balances cannot overflow"),
			Stmt::assume(subSenderBalance.cc)
		});
	}
	thenBlock->addStmt(Stmt::assign(
			context.boogieBalance()->getRefTo(),
			Expr::arrupd(context.boogieBalance()->getRefTo(), context.boogieMsgSender()->getRefTo(), subSenderBalance.expr)));
	thenBlock->addStmt(Stmt::assign(result, Expr::lit(true)));

	// Unsuccessful transfer
	bg::Block::Ref elseBlock = bg::Block::block();
	elseBlock->addStmt(Stmt::assign(result, Expr::lit(false)));

	bg::Block::Ref transferBlock = bg::Block::block();
	// Precondition: there is enough ether to transfer
	auto senderBalanceGEQ = encodeArithBinaryOp(context, nullptr, langutil::Token::GreaterThanOrEqual, sender_bal, amount, 256, false);
	transferBlock->addStmt(Stmt::assume(senderBalanceGEQ.expr));

	// Nondeterministic choice between success and failure
	transferBlock->addStmts({
		Stmt::comment("TODO: call fallback"),
		Stmt::ifelse(Expr::id("*"), thenBlock, elseBlock)
	});

	ProcDeclRef sendProc = Decl::procedure(BOOGIE_SEND, sendParams, sendReturns, {}, {transferBlock});

	sendProc->addAttrs({
		Attr::attr("inline", 1),
		Attr::attr("message", "send")
	});

	return sendProc;
}

string ASTBoogieUtils::dataLocToStr(DataLocation loc)
{
	switch(loc)
	{
	case DataLocation::Storage: return BOOGIE_STOR;
	case DataLocation::Memory: return BOOGIE_MEM;
	case DataLocation::CallData:
		solAssert(false, "CallData storage location is not supported.");
		break;
	default:
		solAssert(false, "Unknown storage location.");
	}
	return "";
}

string ASTBoogieUtils::getConstructorName(ContractDefinition const* contract)
{
	return BOOGIE_CONSTRUCTOR + "#" + toString(contract->id());
}

TypeDeclRef ASTBoogieUtils::mappingType(TypeDeclRef keyType, TypeDeclRef valueType)
{
	return Decl::typee("[" + keyType->getName() + "]" + valueType->getName());
}

std::vector<Attr::Ref> ASTBoogieUtils::createAttrs(SourceLocation const& loc, std::string const& message, Scanner const& scanner)
{
	int srcLine, srcCol;
	tie(srcLine, srcCol) = scanner.translatePositionToLineColumn(loc.start);
	return {
		Attr::attr("sourceloc", loc.source->name(), srcLine + 1, srcCol + 1),
		Attr::attr("message", message)
	};
}

ASTBoogieUtils::ExprWithCC ASTBoogieUtils::encodeArithBinaryOp(BoogieContext& context, ASTNode const* associatedNode, langutil::Token op,
		Expr::Ref lhs, Expr::Ref rhs, unsigned bits, bool isSigned)
{
	Expr::Ref result = nullptr;
	Expr::Ref ecc = nullptr;

	switch(context.encoding())
	{
	case BoogieContext::Encoding::INT:
		switch(op)
		{
		case Token::Add:
		case Token::AssignAdd:
			result = Expr::plus(lhs, rhs); break;
		case Token::Sub:
		case Token::AssignSub:
			result = Expr::minus(lhs, rhs); break;
		case Token::Mul:
		case Token::AssignMul:
			result = Expr::times(lhs, rhs); break;
		// TODO: returning integer division is fine, because Solidity does not support floats
		case Token::Div:
		case Token::AssignDiv:
			result = Expr::intdiv(lhs, rhs); break;
		case Token::Mod:
		case Token::AssignMod:
			result = Expr::mod(lhs, rhs); break;

		case Token::LessThan: result = Expr::lt(lhs, rhs); break;
		case Token::GreaterThan: result = Expr::gt(lhs, rhs); break;
		case Token::LessThanOrEqual: result = Expr::lte(lhs, rhs); break;
		case Token::GreaterThanOrEqual: result = Expr::gte(lhs, rhs); break;

		case Token::Exp:
		{
			auto lhsLit = dynamic_pointer_cast<IntLit const>(lhs);
			auto rhsLit = dynamic_pointer_cast<IntLit const>(rhs);
			if (lhsLit && rhsLit)
				result = Expr::lit(boost::multiprecision::pow(lhsLit->getVal(), rhsLit->getVal().convert_to<unsigned>()));
			else
			{
				context.reportError(associatedNode, "Exponentiation is not supported in 'int' encoding");
				result = Expr::id(ERR_EXPR);
			}
			break;
		}
		default:
			context.reportError(associatedNode, string("Unsupported binary operator in 'int' encoding ") + TokenTraits::toString(op));
			result = Expr::id(ERR_EXPR);
		}
		break;
	case BoogieContext::Encoding::BV:
	{
		string name;
		string retType;

		switch (op)
		{
		case Token::Add:
		case Token::AssignAdd:
			result = context.bvAdd(bits, lhs, rhs); break;
		case Token::Sub:
		case Token::AssignSub:
			result = context.bvSub(bits, lhs, rhs); break;
		case Token::Mul:
		case Token::AssignMul:
			result = context.bvMul(bits, lhs, rhs); break;
		case Token::Div:
		case Token::AssignDiv:
			if (isSigned)
				result = context.bvSDiv(bits, lhs, rhs);
			else
				result = context.bvUDiv(bits, lhs, rhs);
			break;

		case Token::BitAnd:
		case Token::AssignBitAnd:
			result = context.bvAnd(bits, lhs, rhs); break;
		case Token::BitOr:
		case Token::AssignBitOr:
			result = context.bvOr(bits, lhs, rhs); break;
		case Token::BitXor:
		case Token::AssignBitXor:
			result = context.bvXor(bits, lhs, rhs); break;
		case Token::SAR:
		case Token::AssignSar:
			if (isSigned)
				result = context.bvAShr(bits, lhs, rhs);
			else
				result = context.bvLShr(bits, lhs, rhs);
			break;
		case Token::SHL:
		case Token::AssignShl:
			result = context.bvShl(bits, lhs, rhs); break;

		case Token::LessThan:
			if (isSigned)
				result = context.bvSlt(bits, lhs, rhs);
			else
				result = context.bvUlt(bits, lhs, rhs);
			break;
		case Token::GreaterThan:
			if (isSigned)
				result = context.bvSgt(bits, lhs, rhs);
			else
				result = context.bvUgt(bits, lhs, rhs);
			break;
		case Token::LessThanOrEqual:
			if (isSigned)
				result = context.bvSle(bits, lhs, rhs);
			else
				result = context.bvUle(bits, lhs, rhs);
			break;
		case Token::GreaterThanOrEqual:
			if (isSigned)
				result = context.bvSge(bits, lhs, rhs);
			else
				result = context.bvUge(bits, lhs, rhs);
			break;
		case Token::Exp:
		{
			auto lhsLit = dynamic_pointer_cast<BvLit const>(lhs);
			auto rhsLit = dynamic_pointer_cast<BvLit const>(rhs);
			if (lhsLit && rhsLit)
			{
				auto power = boost::multiprecision::pow(boogie::bigint(lhsLit->getVal()),
						boogie::bigint(rhsLit->getVal()).convert_to<unsigned>());
				result = context.intLit(power % boost::multiprecision::pow(boogie::bigint(2), bits), bits);
			}
			else
			{
				context.reportError(associatedNode, "Exponentiation is not supported in 'bv' encoding");
				result = Expr::id(ERR_EXPR);
			}
			break;
		}
		default:
			context.reportError(associatedNode, string("Unsupported binary operator in 'bv' encoding ") + TokenTraits::toString(op));
			result = Expr::id(ERR_EXPR);
		}
		break;
	}
	case BoogieContext::Encoding::MOD:
	{
		auto modulo = Expr::lit(boost::multiprecision::pow(bg::bigint(2), bits));
		auto largestSigned = Expr::lit(boost::multiprecision::pow(bg::bigint(2), bits - 1) - 1);
		auto smallestSigned = Expr::lit(-boost::multiprecision::pow(bg::bigint(2), bits - 1));
		switch(op)
		{
		case Token::Add:
		case Token::AssignAdd:
		{
			auto sum = Expr::plus(lhs, rhs);
			if (isSigned)
				result = Expr::cond(Expr::gt(sum, largestSigned),
					Expr::minus(sum, modulo),
					Expr::cond(Expr::lt(sum, smallestSigned), Expr::plus(sum, modulo), sum));
			else
				result = Expr::cond(Expr::gte(sum, modulo), Expr::minus(sum, modulo), sum);
			ecc = Expr::eq(sum, result);
			break;
		}
		case Token::Sub:
		case Token::AssignSub:
		{
			auto diff = Expr::minus(lhs, rhs);
			if (isSigned)
				result = Expr::cond(Expr::gt(diff, largestSigned),
					Expr::minus(diff, modulo),
					Expr::cond(Expr::lt(diff, smallestSigned), Expr::plus(diff, modulo), diff));
			else
				result = Expr::cond(Expr::gte(lhs, rhs), diff, Expr::plus(diff, modulo));
			ecc = Expr::eq(diff, result);
			break;
		}
		case Token::Mul:
		case Token::AssignMul:
		{
			auto prod = Expr::times(lhs, rhs);
			if (isSigned)
			{
				auto lhs1 = Expr::cond(Expr::gte(lhs, Expr::lit((long)0)), lhs, Expr::plus(modulo, lhs));
				auto rhs1 = Expr::cond(Expr::gte(rhs, Expr::lit((long)0)), rhs, Expr::plus(modulo, rhs));
				auto prod = Expr::mod(Expr::times(lhs1, rhs1), modulo);
				result = Expr::cond(Expr::gt(prod, largestSigned), Expr::minus(prod, modulo), prod);
			}
			else
			{
				result = Expr::cond(Expr::gte(prod, modulo), Expr::mod(prod, modulo), prod);
			}
			ecc = Expr::eq(prod, result);
			break;
		}
		case Token::Div:
		case Token::AssignDiv:
		{
			auto div = Expr::intdiv(lhs, rhs);
			if (isSigned)
				result = Expr::cond(Expr::gt(div, largestSigned),
					Expr::minus(div, modulo),
					Expr::cond(Expr::lt(div, smallestSigned), Expr::plus(div, modulo), div));
			else
				result = div;
			ecc = Expr::eq(div, result);
			break;
		}

		case Token::LessThan:
			result = Expr::lt(lhs, rhs);
			break;
		case Token::GreaterThan:
			result = Expr::gt(lhs, rhs);
			break;
		case Token::LessThanOrEqual:
			result = Expr::lte(lhs, rhs);
			break;
		case Token::GreaterThanOrEqual:
			result = Expr::gte(lhs, rhs);
			break;
		case Token::Exp:
		{
			auto lhsLit = dynamic_pointer_cast<IntLit const>(lhs);
			auto rhsLit = dynamic_pointer_cast<IntLit const>(rhs);
			if (lhsLit && rhsLit)
			{
				auto power = boost::multiprecision::pow(lhsLit->getVal(),rhsLit->getVal().convert_to<unsigned>());
				result = context.intLit(power % boost::multiprecision::pow(boogie::bigint(2), isSigned ? bits - 1 : bits), bits);
				ecc = Expr::eq(context.intLit(power, bits), result);
			}
			else
			{
				context.reportError(associatedNode, "Exponentiation is not supported in 'mod' encoding");
				result = Expr::id(ERR_EXPR);
			}
			break;
		}
		default:
			context.reportError(associatedNode, string("Unsupported binary operator in 'mod' encoding ") + TokenTraits::toString(op));
			result = Expr::id(ERR_EXPR);
		}
		break;
	}
	default:
		solAssert(false, "Unknown arithmetic encoding");
	}

	assert(result != nullptr);
	return ExprWithCC{result, ecc};
}

ASTBoogieUtils::ExprWithCC ASTBoogieUtils::encodeArithUnaryOp(BoogieContext& context, ASTNode const* associatedNode, Token op,
		Expr::Ref subExpr, unsigned bits, bool isSigned)
{
	Expr::Ref result = nullptr;
	Expr::Ref ecc = nullptr;

	switch(context.encoding())
	{
	case BoogieContext::Encoding::INT:
		switch(op)
		{
		case Token::Sub: result = Expr::neg(subExpr); break;
		default:
			context.reportError(associatedNode, string("Unsupported unary operator in 'int' encoding ") + TokenTraits::toString(op));
			result = Expr::id(ERR_EXPR);
			break;
		}
		break;

	case BoogieContext::Encoding::BV:
		{
			string name("");
			switch (op)
			{
			case Token::Sub: result = context.bvNeg(bits, subExpr); break;
			case Token::BitNot: result = context.bvNot(bits, subExpr); break;
			default:
				context.reportError(associatedNode, string("Unsupported unary operator in 'bv' encoding ") + TokenTraits::toString(op));
				result = Expr::id(ERR_EXPR);
				break;
			}
		}
		break;

	case BoogieContext::Encoding::MOD:
		switch(op)
		{
		case Token::Sub:
		{
			auto sub = Expr::neg(subExpr);
			if (isSigned)
			{
				auto smallestSigned = Expr::lit(-boost::multiprecision::pow(bg::bigint(2), bits - 1));
				result = Expr::cond(Expr::eq(subExpr, smallestSigned),
						smallestSigned,
						sub);
			}
			else
			{
				auto modulo = Expr::lit(boost::multiprecision::pow(bg::bigint(2), bits));
				result = Expr::cond(Expr::eq(subExpr, Expr::lit((long)0)),
						Expr::lit((long)0),
						Expr::minus(modulo, subExpr));
			}
			ecc = Expr::eq(sub, result);
			break;
		}
		default:
			context.reportError(associatedNode, string("Unsupported unary operator in 'mod' encoding ") + TokenTraits::toString(op));
			result = Expr::id(ERR_EXPR);
			break;
		}
		break;

	default:
		solAssert(false, "Unknown arithmetic encoding");
	}
	assert(result != nullptr);
	return ExprWithCC{result, ecc};
}

bool ASTBoogieUtils::isBitPreciseType(TypePointer type)
{
	switch (type->category())
	{
	case Type::Category::Integer:
	case Type::Category::FixedBytes:
	case Type::Category::Enum:
		return true;
	case Type::Category::Tuple:
	{
		auto tupleType = dynamic_cast<TupleType const*>(type);
		for (auto e: tupleType->components())
			if (e && !isBitPreciseType(e))
				return false;
		return true;
	}
	default:
		return false;
	}
	return true;
}

unsigned ASTBoogieUtils::getBits(TypePointer type)
{
	if (auto intType = dynamic_cast<IntegerType const*>(type))
		return intType->numBits();
	if (dynamic_cast<EnumType const*>(type))
		return 256;

	solAssert(false, "Trying to get bits for non-bitprecise type");
	return 0;
}

bool ASTBoogieUtils::isSigned(TypePointer type)
{
	if (auto intType = dynamic_cast<IntegerType const*>(type))
		return intType->isSigned();
	if (dynamic_cast<EnumType const*>(type))
		return false;

	solAssert(false, "Trying to get sign for non-bitprecise type");
	return false;
}

Expr::Ref ASTBoogieUtils::checkImplicitBvConversion(Expr::Ref expr, TypePointer exprType, TypePointer targetType, BoogieContext& context)
{
	solAssert(exprType, "");
	solAssert(targetType, "");

	// If tuples, do it element-wise
	if (targetType->category() == Type::Category::Tuple)
	{
		std::vector<Expr::Ref> elements;

		auto targetTupleType = dynamic_cast<TupleType const*>(targetType);
		auto exprTypleType = dynamic_cast<TupleType const*>(exprType);
		auto exprTuple = std::dynamic_pointer_cast<TupleExpr const>(expr);

		for (size_t i = 0; i < targetTupleType->components().size(); i ++)
		{
			auto expr_i = exprTuple->elements()[i];
			auto exprType_i = exprTypleType->components()[i];
			auto targetType_i = targetTupleType->components()[i];
			auto result_i = targetType_i ?
					checkImplicitBvConversion(expr_i, exprType_i, targetType_i, context) : nullptr;
			elements.push_back(result_i);
		}

		return Expr::tuple(elements);
	}

	if (isBitPreciseType(targetType))
	{
		unsigned targetBits = getBits(targetType);
		// Create bitvector from literals
		if (auto exprLit = dynamic_pointer_cast<IntLit const>(expr))
		{
			if (exprLit->getVal() < 0) // Negative literals are tricky
				return context.bvNeg(targetBits, Expr::lit(-exprLit->getVal(), targetBits));
			else
				return Expr::lit(exprLit->getVal(), targetBits);
		}
		else if (isBitPreciseType(exprType))
		{
			unsigned exprBits = getBits(exprType);
			bool targetSigned = isSigned(targetType);
			bool exprSigned = isSigned(exprType);

			// Nothing to do if size and signedness is the same
			if (targetBits == exprBits && targetSigned == exprSigned)
				return expr;
			// Conversion to smaller type should have already been detected by the compiler
			solAssert (targetBits >= exprBits, "Implicit conversion to smaller type");

			if (!exprSigned) // Unsigned can be converted to larger (signed or unsigned) with zero extension
				return context.bvZeroExt(expr, exprBits, targetBits);
			else if (targetSigned) // Signed can only be converted to signed with sign extension
				return context.bvSignExt(expr, exprBits, targetBits);
			else // Signed to unsigned should have already been detected by the compiler
			{
				solAssert(false, "Implicit conversion from signed to unsigned");
				return nullptr;
			}
		}
	}

	return expr;
}

Expr::Ref ASTBoogieUtils::checkExplicitBvConversion(Expr::Ref expr, TypePointer exprType, TypePointer targetType, BoogieContext& context)
{
	// Do nothing if any of the types is unknown
	if (!targetType || !exprType)
		return expr;

	if (isBitPreciseType(targetType))
	{
		unsigned targetBits = getBits(targetType);
		// Literals can be handled by implicit conversion
		if (dynamic_pointer_cast<IntLit const>(expr))
		{
			return checkImplicitBvConversion(expr, exprType, targetType, context);
		}
		else if (isBitPreciseType(exprType))
		{
			unsigned exprBits = getBits(exprType);
			bool targetSigned = isSigned(targetType);
			bool exprSigned = isSigned(exprType);

			// Check if explicit conversion is really needed:
			// - converting to smaller size
			// - converting from signed to unsigned
			// - converting from unsigned to same size signed
			if (targetBits < exprBits || (exprSigned && !targetSigned) || (targetBits == exprBits && !exprSigned && targetSigned))
			{
				// Nothing to do for same size, since Boogie bitvectors do not have signs
				if (targetBits == exprBits)
					return expr;
				// For larger sizes, sign extension is done
				else if (targetBits > exprBits)
					return context.bvSignExt(expr, exprBits, targetBits);
				// For smaller sizes, higher-order bits are discarded
				else
					return context.bvExtract(expr, exprBits,targetBits-1, 0);
			}
			// Otherwise the implicit will handle it
			else
			{
				return checkImplicitBvConversion(expr, exprType, targetType, context);
			}
		}
	}

	return expr;
}

Expr::Ref ASTBoogieUtils::getTCCforExpr(Expr::Ref expr, TypePointer tp)
{
	// For enums we know the number of members
	if (tp->category() == Type::Category::Enum)
	{
		auto enumTp = dynamic_cast<EnumType const *>(tp);
		solAssert(enumTp, "");
		return Expr::and_(
				Expr::lte(Expr::lit((long)0), expr),
				Expr::lt(expr, Expr::lit(enumTp->enumDefinition().members().size())));

	}
	// Otherwise get smallest and largest
	if (isBitPreciseType(tp))
	{
		unsigned bits = getBits(tp);
		if (isSigned(tp))
		{
			auto largestSigned = Expr::lit(boost::multiprecision::pow(bg::bigint(2), bits - 1) - 1);
			auto smallestSigned = Expr::lit(-boost::multiprecision::pow(bg::bigint(2), bits - 1));
			return Expr::and_(
					Expr::lte(smallestSigned, expr),
					Expr::lte(expr, largestSigned));
		}
		else
		{
			auto largestUnsigned = Expr::lit(boost::multiprecision::pow(bg::bigint(2), bits) - 1);
			auto smallestUnsigned = Expr::lit(long(0));
			return Expr::and_(
					Expr::lte(smallestUnsigned, expr),
					Expr::lte(expr, largestUnsigned));
		}
	}
	return Expr::lit(true);
}

bool ASTBoogieUtils::isStateVar(Declaration const *decl)
{
	if (auto varDecl = dynamic_cast<VariableDeclaration const*>(decl))
		return varDecl->isStateVariable();

	return false;
}


Expr::Ref ASTBoogieUtils::selectToUpdate(Expr::Ref sel, Expr::Ref value)
{
	if (auto selExpr = dynamic_pointer_cast<SelExpr const>(sel))
	{
		if (auto base = dynamic_pointer_cast<SelExpr const>(selExpr->getBase()))
			return selectToUpdate(base, selExpr->toUpdate(value));
		else
			return selExpr->toUpdate(value);
	}
	solAssert(false, "Expected datatype/array select");
	return nullptr;
}

}
}
