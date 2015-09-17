#include <map>

#include <llvm/IR/Value.h>
#include <llvm/IR/Instruction.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/raw_ostream.h>

#include "LLVMNode.h"
#include "LLVMDependenceGraph.h"
#include "ReachingDefs.h"
#include "AnalysisGeneric.h"
#include "DefMap.h"

#include "analysis/DFS.h"
#include "llvm-debug.h"

using namespace llvm;

namespace dg {
namespace analysis {

LLVMReachingDefsAnalysis::LLVMReachingDefsAnalysis(LLVMDependenceGraph *dg)
    : DataFlowAnalysis<LLVMNode>(dg->getEntryBB(), DATAFLOW_INTERPROCEDURAL),
      dg(dg)
{
    Module *m = dg->getModule();
    // set data layout
    DL = m->getDataLayout();
}

Pointer LLVMReachingDefsAnalysis::getConstantExprPointer(const ConstantExpr *CE)
{
    return dg::analysis::getConstantExprPointer(CE, dg, DL);
}

// FIXME don't duplicate the code from DefUse.cpp
static DefMap *getDefMap(LLVMNode *n)
{
    DefMap *r = n->getData<DefMap>();
    if (!r) {
        r = new DefMap();
        n->setData(r);
    }

    // must always have
    assert(r);

    return r;
}

/// --------------------------------------------------
//   Reaching definitions analysis
/// --------------------------------------------------

static bool handleParam(LLVMNode *node, LLVMNode *to,
                        DefMap *df, DefMap *subgraph_df)
{
    bool changed = false;
    for (const Pointer& ptr : node->getPointsTo()) {
        ValuesSetT& defs = subgraph_df->get(ptr);
        if (defs.empty())
            continue;

        changed |= df->add(ptr, to);
    }

    return changed;
}

static bool handleParamsGlobals(LLVMDependenceGraph *dg,
                                LLVMDGParameters *params,
                                DefMap *df, DefMap *subgraph_df)
{
    bool changed = false;
    for (auto I = params->global_begin(), E = params->global_end(); I != E; ++I) {
        LLVMDGParameter& p = I->second;

        // get the global node, it contains the points-to set
        LLVMNode *glob = dg->getNode(I->first);
        if (!glob) {
            errs() << "ERR: no global node for param\n";
            continue;
        }

        changed |= handleParam(glob, p.out, df, subgraph_df);
    }

    return changed;
}

static bool handleParams(LLVMNode *callNode, LLVMDGParameters *params,
                         DefMap *df, DefMap *subgraph_df)
{
    bool changed = false;

    // operand[0] is the called func
    for (int i = 1, e = callNode->getOperandsNum(); i < e; ++i) {
        LLVMNode *op = callNode->getOperand(i);
        if (!op)
            continue;

        if (!op->isPointerTy())
            continue;

        LLVMDGParameter *p = params->find(op->getKey());
        if (!p) {
            DBG("ERR: no actual param for " << *op->getKey());
            continue;
        }

        changed |= handleParam(op, p->out, df, subgraph_df);
    }

    return changed;
}

static bool handleParams(LLVMNode *callNode, DefMap *df, DefMap *subgraph_df)
{
    bool changed = false;

    // get actual parameters (operands) and for every pointer in there
    // check if the memory location it points to gets defined
    // in the subprocedure
    LLVMDGParameters *params = callNode->getParameters();
    // if we have params, process params
    if (!params)
        return false;

    changed |= handleParams(callNode, params, df, subgraph_df);
    changed |= handleParamsGlobals(callNode->getDG(), params, df, subgraph_df);

    return changed;
}

static bool handleCallInst(LLVMDependenceGraph *graph,
                           LLVMNode *callNode, DefMap *df)
{
    bool changed = false;

    LLVMNode *exitNode = graph->getExit();
    assert(exitNode && "No exit node in subgraph");

    DefMap *subgraph_df = getDefMap(exitNode);
    // now handle all parameters
    // and global variables that are as parameters
    changed |= handleParams(callNode, df, subgraph_df);

    return changed;
}

static bool handleCallInst(LLVMNode *callNode, DefMap *df)
{
    bool changed = false;

    for (LLVMDependenceGraph *subgraph : callNode->getSubgraphs())
        changed |= handleCallInst(subgraph, callNode, df);

    return changed;
}

static bool handleStoreInst(LLVMNode *storeNode, DefMap *df,
                            PointsToSetT *&strong_update)
{
    bool changed = false;
    LLVMNode *ptrNode = storeNode->getOperand(0);
    assert(ptrNode && "No pointer operand");

    // update definitions
    PointsToSetT& S = ptrNode->getPointsTo();
    if (S.size() == 1) {// strong update
        changed |= df->update(*S.begin(), storeNode);
        strong_update = &S;
    } else { // weak update
        for (const Pointer& ptr : ptrNode->getPointsTo())
            changed |= df->add(ptr, storeNode);
    }

    return changed;
}

bool LLVMReachingDefsAnalysis::runOnNode(LLVMNode *node)
{
    bool changed = false;
    // pointers that should not be updated
    // because they were updated strongly
    PointsToSetT *strong_update = nullptr;

    // update states according to predcessors
    DefMap *df = getDefMap(node);
    LLVMNode *pred = node->getPredcessor();
    if (pred) {
        const Value *predVal = pred->getKey();
        // if the predcessor is StoreInst, it add and may kill some definitions
        if (isa<StoreInst>(predVal))
            changed |= dg::analysis::handleStoreInst(pred, df, strong_update);
        // call inst may add some definitions to (StoreInst in subgraph)
        else if (isa<CallInst>(predVal))
            changed |= dg::analysis::handleCallInst(pred, df);

        changed |= df->merge(getDefMap(pred), strong_update);
    } else { // BB predcessors
        LLVMBBlock *BB = node->getBBlock();
        assert(BB && "Node has no BB");

        for (auto predBB : BB->predcessors()) {
            pred = predBB->getLastNode();
            assert(pred && "BB has no last node");

            const Value *predVal = pred->getKey();

            if (isa<StoreInst>(predVal))
                changed |= dg::analysis::handleStoreInst(pred, df, strong_update);
            else if (isa<CallInst>(predVal))
                changed |= dg::analysis::handleCallInst(pred, df);

            df->merge(getDefMap(pred), nullptr);
        }
    }

    return changed;
}

} // namespace analysis
} // namespace dg