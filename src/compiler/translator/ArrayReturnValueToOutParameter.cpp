//
// Copyright (c) 2002-2015 The ANGLE Project Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// The ArrayReturnValueToOutParameter function changes return values of an array type to out
// parameters in function definitions, prototypes, and call sites.

#include "compiler/translator/ArrayReturnValueToOutParameter.h"

#include <map>

#include "compiler/translator/IntermNode_util.h"
#include "compiler/translator/IntermTraverse.h"
#include "compiler/translator/StaticType.h"
#include "compiler/translator/SymbolTable.h"

namespace sh
{

namespace
{

void CopyAggregateChildren(TIntermAggregateBase *from, TIntermAggregateBase *to)
{
    const TIntermSequence *fromSequence = from->getSequence();
    for (size_t ii = 0; ii < fromSequence->size(); ++ii)
    {
        to->getSequence()->push_back(fromSequence->at(ii));
    }
}

class ArrayReturnValueToOutParameterTraverser : private TIntermTraverser
{
  public:
    static void apply(TIntermNode *root, TSymbolTable *symbolTable);

  private:
    ArrayReturnValueToOutParameterTraverser(TSymbolTable *symbolTable);

    bool visitFunctionPrototype(Visit visit, TIntermFunctionPrototype *node) override;
    bool visitFunctionDefinition(Visit visit, TIntermFunctionDefinition *node) override;
    bool visitAggregate(Visit visit, TIntermAggregate *node) override;
    bool visitBranch(Visit visit, TIntermBranch *node) override;
    bool visitBinary(Visit visit, TIntermBinary *node) override;

    TIntermAggregate *createReplacementCall(TIntermAggregate *originalCall,
                                            TIntermTyped *returnValueTarget);

    // Set when traversal is inside a function with array return value.
    TIntermFunctionDefinition *mFunctionWithArrayReturnValue;

    struct ChangedFunction
    {
        const TVariable *returnValueVariable;
        const TFunction *func;
    };

    // Map from function symbol ids to the changed function.
    std::map<int, ChangedFunction> mChangedFunctions;

    const TString *const mReturnValueVariableName;
};

TIntermAggregate *ArrayReturnValueToOutParameterTraverser::createReplacementCall(
    TIntermAggregate *originalCall,
    TIntermTyped *returnValueTarget)
{
    TIntermSequence *replacementArguments = new TIntermSequence();
    TIntermSequence *originalArguments    = originalCall->getSequence();
    for (auto &arg : *originalArguments)
    {
        replacementArguments->push_back(arg);
    }
    replacementArguments->push_back(returnValueTarget);
    ASSERT(originalCall->getFunction());
    const TSymbolUniqueId &originalId = originalCall->getFunction()->uniqueId();
    TIntermAggregate *replacementCall = TIntermAggregate::CreateFunctionCall(
        *mChangedFunctions[originalId.get()].func, replacementArguments);
    replacementCall->setLine(originalCall->getLine());
    return replacementCall;
}

void ArrayReturnValueToOutParameterTraverser::apply(TIntermNode *root, TSymbolTable *symbolTable)
{
    ArrayReturnValueToOutParameterTraverser arrayReturnValueToOutParam(symbolTable);
    root->traverse(&arrayReturnValueToOutParam);
    arrayReturnValueToOutParam.updateTree();
}

ArrayReturnValueToOutParameterTraverser::ArrayReturnValueToOutParameterTraverser(
    TSymbolTable *symbolTable)
    : TIntermTraverser(true, false, true, symbolTable),
      mFunctionWithArrayReturnValue(nullptr),
      mReturnValueVariableName(NewPoolTString("angle_return"))
{
}

bool ArrayReturnValueToOutParameterTraverser::visitFunctionDefinition(
    Visit visit,
    TIntermFunctionDefinition *node)
{
    if (node->getFunctionPrototype()->isArray() && visit == PreVisit)
    {
        // Replacing the function header is done on visitFunctionPrototype().
        mFunctionWithArrayReturnValue = node;
    }
    if (visit == PostVisit)
    {
        mFunctionWithArrayReturnValue = nullptr;
    }
    return true;
}

bool ArrayReturnValueToOutParameterTraverser::visitFunctionPrototype(Visit visit,
                                                                     TIntermFunctionPrototype *node)
{
    if (visit == PreVisit && node->isArray())
    {
        // Replace the whole prototype node with another node that has the out parameter
        // added. Also set the function to return void.
        const TSymbolUniqueId &functionId = node->getFunction()->uniqueId();
        if (mChangedFunctions.find(functionId.get()) == mChangedFunctions.end())
        {
            TType returnValueVariableType(node->getType());
            returnValueVariableType.setQualifier(EvqOut);
            ChangedFunction changedFunction;
            changedFunction.returnValueVariable =
                new TVariable(mSymbolTable, mReturnValueVariableName, returnValueVariableType,
                              SymbolType::AngleInternal);
            changedFunction.func = new TFunction(mSymbolTable, &node->getFunction()->name(),
                                                 StaticType::GetBasic<EbtVoid>(),
                                                 node->getFunction()->symbolType(), false);
            mChangedFunctions[functionId.get()] = changedFunction;
        }
        TIntermFunctionPrototype *replacement =
            new TIntermFunctionPrototype(mChangedFunctions[functionId.get()].func);
        CopyAggregateChildren(node, replacement);
        replacement->getSequence()->push_back(
            new TIntermSymbol(mChangedFunctions[functionId.get()].returnValueVariable));
        replacement->setLine(node->getLine());

        queueReplacement(replacement, OriginalNode::IS_DROPPED);
    }
    return false;
}

bool ArrayReturnValueToOutParameterTraverser::visitAggregate(Visit visit, TIntermAggregate *node)
{
    ASSERT(!node->isArray() || node->getOp() != EOpCallInternalRawFunction);
    if (visit == PreVisit && node->isArray() && node->getOp() == EOpCallFunctionInAST)
    {
        // Handle call sites where the returned array is not assigned.
        // Examples where f() is a function returning an array:
        // 1. f();
        // 2. another_array == f();
        // 3. another_function(f());
        // 4. return f();
        // Cases 2 to 4 are already converted to simpler cases by
        // SeparateExpressionsReturningArrays, so we only need to worry about the case where a
        // function call returning an array forms an expression by itself.
        TIntermBlock *parentBlock = getParentNode()->getAsBlock();
        if (parentBlock)
        {
            // replace
            //   f();
            // with
            //   type s0[size]; f(s0);
            TIntermSequence replacements;

            // type s0[size];
            TIntermDeclaration *returnValueDeclaration = nullptr;
            TVariable *returnValue = DeclareTempVariable(mSymbolTable, node->getType(),
                                                         EvqTemporary, &returnValueDeclaration);
            replacements.push_back(returnValueDeclaration);

            // f(s0);
            TIntermSymbol *returnValueSymbol = CreateTempSymbolNode(returnValue);
            replacements.push_back(createReplacementCall(node, returnValueSymbol));
            mMultiReplacements.push_back(
                NodeReplaceWithMultipleEntry(parentBlock, node, replacements));
        }
        return false;
    }
    return true;
}

bool ArrayReturnValueToOutParameterTraverser::visitBranch(Visit visit, TIntermBranch *node)
{
    if (mFunctionWithArrayReturnValue && node->getFlowOp() == EOpReturn)
    {
        // Instead of returning a value, assign to the out parameter and then return.
        TIntermSequence replacements;

        TIntermTyped *expression = node->getExpression();
        ASSERT(expression != nullptr);
        const TSymbolUniqueId &functionId =
            mFunctionWithArrayReturnValue->getFunction()->uniqueId();
        ASSERT(mChangedFunctions.find(functionId.get()) != mChangedFunctions.end());
        TIntermSymbol *returnValueSymbol =
            new TIntermSymbol(mChangedFunctions[functionId.get()].returnValueVariable);
        TIntermBinary *replacementAssignment =
            new TIntermBinary(EOpAssign, returnValueSymbol, expression);
        replacementAssignment->setLine(expression->getLine());
        replacements.push_back(replacementAssignment);

        TIntermBranch *replacementBranch = new TIntermBranch(EOpReturn, nullptr);
        replacementBranch->setLine(node->getLine());
        replacements.push_back(replacementBranch);

        mMultiReplacements.push_back(
            NodeReplaceWithMultipleEntry(getParentNode()->getAsBlock(), node, replacements));
    }
    return false;
}

bool ArrayReturnValueToOutParameterTraverser::visitBinary(Visit visit, TIntermBinary *node)
{
    if (node->getOp() == EOpAssign && node->getLeft()->isArray())
    {
        TIntermAggregate *rightAgg = node->getRight()->getAsAggregate();
        ASSERT(rightAgg == nullptr || rightAgg->getOp() != EOpCallInternalRawFunction);
        if (rightAgg != nullptr && rightAgg->getOp() == EOpCallFunctionInAST)
        {
            TIntermAggregate *replacementCall = createReplacementCall(rightAgg, node->getLeft());
            queueReplacement(replacementCall, OriginalNode::IS_DROPPED);
        }
    }
    return false;
}

}  // namespace

void ArrayReturnValueToOutParameter(TIntermNode *root, TSymbolTable *symbolTable)
{
    ArrayReturnValueToOutParameterTraverser::apply(root, symbolTable);
}

}  // namespace sh
