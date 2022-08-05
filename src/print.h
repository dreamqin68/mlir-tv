#pragma once

#include "mlir/Dialect/StandardOps/IR/Ops.h"
#include "mlir/IR/BuiltinOps.h"

#include "smt.h"
#include "state.h"
#include "vcgen.h"

#include <vector>

void printOperations(smt::Model m, mlir::FuncOp fn, const State &st);

void printCounterEx(
    smt::Model model, const std::vector<smt::Expr> &params,
    mlir::FuncOp src, mlir::FuncOp tgt,
    const State &st_src, const State &st_tgt,
    VerificationStep step, unsigned retvalidx = -1,
    std::optional<mlir::Type> memElemTy = std::nullopt);