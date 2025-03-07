#include "debug.h"
#include "memory.h"
#include "opts.h"
#include "smt.h"
#include "smtmatchers.h"
#include "utils.h"
#include "value.h"
#include "llvm/Support/raw_ostream.h"

using namespace smt;
using namespace std;


namespace {
string freshName(string &&prefix) {
  static int count = 0;
  return prefix + "#" + to_string(count ++);
}
}

optional<smt::Sort> convertPrimitiveTypeToSort(mlir::Type elemty) {
  if (auto ielemty = mlir::dyn_cast<mlir::IntegerType>(elemty)) {
    return Integer::sort(ielemty.getWidth());
  } else if (auto felemty = mlir::dyn_cast<mlir::FloatType>(elemty)) {
    return Float::sort(felemty);
  } else if (elemty.isIndex()) {
    return Index::sort();
  }

  return {};
}

optional<Expr> getZero(mlir::Type eltType) {
  if (convertPrimitiveTypeToSort(eltType) == nullopt)
    return nullopt;

  if (eltType.isa<mlir::FloatType>())
    return aop::getFpEncoding(eltType).zero();
  else if (eltType.isa<mlir::IntegerType>())
    return Integer(0, eltType.getIntOrFloatBitWidth());
  else if (eltType.isIndex())
    return Index(0);
  return {};
}

optional<Expr> getIdentity(mlir::Type eltType) {
  if (convertPrimitiveTypeToSort(eltType) == nullopt)
    return nullopt;

  if (mlir::isa<mlir::FloatType>(eltType))
    return aop::getFpEncoding(eltType).zero(true);
  else if (mlir::isa<mlir::IntegerType>(eltType))
    return Integer(0, eltType.getIntOrFloatBitWidth());
  else if (eltType.isIndex())
    return Index(0);
  return {};
}

static vector<pair<mlir::ElementsAttr, Tensor>> abstractlyEncodedAttrs;

void resetAbstractlyEncodedAttrs() {
  abstractlyEncodedAttrs.clear();
}


vector<Expr> ShapedValue::getDims(
    const mlir::ShapedType &shapedTy, bool freshVarForUnknownSize,
    optional<vector<Expr>> &&valsForUnknownSz) {
  vector<Expr> dims;

  uint64_t rank = shapedTy.getRank();
  if (rank == 0) {
    // A single element tensor.
    return vector<Expr>{Index(1)};
  }

  dims.reserve(rank);
  unsigned unknownVarIdx = 0;
  for (unsigned i = 0; i < rank; ++i) {
    int64_t sz = shapedTy.getDimSize(i);
    if (sz == mlir::ShapedType::kDynamic) {
      if (freshVarForUnknownSize) {
        dims.emplace_back(Index::var("dim", VarType::FRESH));
      } else if (valsForUnknownSz) {
        dims.emplace_back(std::move((*valsForUnknownSz)[unknownVarIdx++]));
      } else {
        llvm_unreachable("Don't know what to do with a dimension of "
                         "an unknown size");
      }
    } else {
      assert(sz >= 0);
      dims.push_back(Index((uint64_t)sz));
    }
  }

  return dims;
}

Index::Index(unsigned i): e(Expr::mkBV(i, BITS)) {}

Sort Index::sort() {
  return Sort::bvSort(BITS);
}

Index Index::one() { return Index(1); }
Index Index::zero() { return Index(0); }
Index Index::var(string &&name, VarType varty) {
  Index i(0);
  switch(varty) {
  case VarType::BOUND:
    static unsigned varCount = 0;
    i = {Expr::mkVar(Index::sort(), std::move(name) + "#" + to_string(varCount++),
            true)};
    break;
  case VarType::UNBOUND:
    i = {Expr::mkVar(Index::sort(), std::move(name), false)};
    break;
  case VarType::FRESH:
    i = {Expr::mkFreshVar(Index::sort(), std::move(name))};
    break;
  default:
    llvm_unreachable("Unknown case");
  }
  smart_assert(i.e.isVar(), "Index::var must return a variable, but got " << i.e);
  return i;
}
vector<Expr> Index::boundIndexVars(unsigned n) {
  vector<Expr> idxs;
  for (unsigned i = 0; i < n; i ++) {
    idxs.push_back(Index::var("i", VarType::BOUND));
  }
  return idxs;
}


llvm::raw_ostream& operator<<(llvm::raw_ostream& os, const Index &i) {
  os << or_omit((Expr)i);
  return os;
};

pair<Expr, vector<Expr>> Index::refines(const Index &other) const {
  return {(Expr) other == (Expr) *this, {}};
}

Index Index::eval(Model m) const {
  return Index(m.eval(e, true).simplify());
}

optional<Sort> Float::sort(mlir::Type t) {
  if (t.isF32()) {
    return aop::getFloatEncoding().sort();
  } else if (t.isF64()) {
    return aop::getDoubleEncoding().sort();
  }
  return nullopt;
}

Float Float::constant(const llvm::APFloat &apf, mlir::Type ty) {
  assert(sort(ty) != nullopt);

  return {aop::getFpEncoding(ty).constant(apf), ty};
}

Float Float::one(mlir::Type t) {
  if (t.isF32()) {
    return constant(llvm::APFloat(1.0f), t);
  } else if (t.isF64()) {
    return constant(llvm::APFloat(1.0), t);
  }

  throw UnsupportedException(t, "Unknown float type");
}

Float Float::castFromSignedInt(Integer &integer, mlir::Type ty) {
  assert(sort(ty) != nullopt);

  return {aop::getFpEncoding(ty).castFromSignedInt(integer), ty};
}


Float Float::exp(const Float &x) {
  return {aop::getFpEncoding(x.type).exp(x.e), x.type};
}

Sort Float::sortFloat32() {
  return aop::getFloatEncoding().sort();
}

Float Float::var(string &&name, mlir::Type ty, VarType varty) {
  switch(varty) {
  case VarType::BOUND:
  case VarType::UNBOUND:
    return {Expr::mkVar(*Float::sort(ty), std::move(name), varty == VarType::BOUND),
            ty};
  case VarType::FRESH:
    return {Expr::mkFreshVar(*Float::sort(ty), std::move(name)), ty};
  }
  llvm_unreachable("Unknown case");
}

llvm::raw_ostream& operator<<(llvm::raw_ostream& os, const Float &f) {
  Expr e = f;
  if (e.sort().isFPASort()) {
    os << e.simplify();
  } else {
    auto vec = aop::getFpEncoding(f.type).possibleConsts(e);
    if (!vec.empty()) {
      llvm::SmallVector<char, 16> str;
      vec[0].toString(str);
      os << str;
      for (unsigned i = 1; i < vec.size(); ++i) {
        str.clear();
        vec[i].toString(str);
        os << " or " << str;
      }
    } else {
      os << "unknown (" << or_omit((Expr)f) << ")";
    }
  }
  return os;
};

pair<Expr, vector<Expr>> Float::refines(const Float &other) const {
  auto nan1 = aop::getFpEncoding(type).isnan(e);
  auto nan2 = aop::getFpEncoding(type).isnan(other.e);
  return {
    Expr::mkIte(nan1 | nan2, nan1 == nan2, (Expr) other == (Expr) *this), {}};
}

Float Float::eval(Model m) const {
  return Float(m.eval(e, true).simplify(), type);
}

Float Float::add(const Float &b) const {
  return Float(aop::getFpEncoding(type).add(e, b.e), type);
}

Float Float::mul(const Float &b) const {
  return Float(aop::getFpEncoding(type).mul(e, b.e), type);
}

Float Float::div(const Float &b) const {
  return Float(aop::getFpEncoding(type).div(e, b.e), type);
}

Integer Float::cmp(const mlir::arith::CmpFPredicate pred, const Float &b)
    const {
  return Integer(aop::getFpEncoding(type).cmp(pred, e, b.e));
}

Float Float::abs() const {
  return Float(aop::getFpEncoding(type).abs(e), type);
}

Float Float::neg() const {
  return Float(aop::getFpEncoding(type).neg(e), type);
}

Float Float::extend(const mlir::Type &tgt_type) const {
  auto& src_encoding = aop::getFpEncoding(type);
  auto& tgt_encoding = aop::getFpEncoding(tgt_type);
  return Float(src_encoding.extend(e, tgt_encoding), tgt_type);
}

Float Float::truncate(const mlir::Type &tgt_type) const {
  auto& src_encoding = aop::getFpEncoding(type);
  auto& tgt_encoding = aop::getFpEncoding(tgt_type);
  return Float(src_encoding.truncate(e, tgt_encoding), tgt_type);
}



Integer::Integer(int64_t i, unsigned bw):
  e(Expr::mkBV(i, bw)) {}

Integer::Integer(const llvm::APInt &api):
  Integer(api.getSExtValue(), api.getBitWidth()) {}

Sort Integer::sort(unsigned sz) {
  return Sort::bvSort(sz);
}

Integer Integer::var(string &&name, unsigned bw, VarType varty) {
  switch(varty) {
  case VarType::BOUND:
  case VarType::UNBOUND:
    return {Expr::mkVar(Sort::bvSort(bw), std::move(name), varty == VarType::BOUND)};
  case VarType::FRESH:
    return {Expr::mkFreshVar(Sort::bvSort(bw), std::move(name))};
  }
  llvm_unreachable("Unknown case");
}

Integer Integer::boolTrue() { return Integer(1, 1); }
Integer Integer::boolFalse() { return Integer(0, 1); }

llvm::raw_ostream& operator<<(llvm::raw_ostream& os, const Integer &i) {
  os << or_omit((Expr)i);
  return os;
};

pair<Expr, vector<Expr>> Integer::refines(const Integer &other) const {
  smart_assert(bitwidth() == other.bitwidth(),
      "To check refinement of two integer values, their bitwidth must be "
      "equal, but got " << bitwidth() << " != " << other.bitwidth());
  return {(Expr) other == (Expr) *this, {}};
}

Integer Integer::eval(Model m) const {
  return Integer(m.eval(e, true).simplify());
}

pair<vector<smt::Expr>, smt::Expr> ShapedValue::conv(
    const ShapedValue &filter,
    const vector<Expr> &strides,
    const vector<Expr> &dilations,
    ConvLayout convLayout,
    function<optional<Expr>(vector<Expr> &)> &&getInitValue) const {
  // 1. NHWC_HWCF:
  //   input: Batch_size x Dim_0 x Dim_1 .. x Dim_{n-1} x Input_channel
  //   filter: Dim_0 x Dim_1 .. x Dim_{n-1} x Input_channel x Output_channel
  //   output: Batch_size x Dim_0 x Dim_1 .. x Dim_{n-1} x Output_channel
  // 2. NCHW_FCHW:
  //   input: Batch_size x Input_channel x Dim_0 x Dim_1 .. x Dim_{n-1}
  //   filter: Output_channel x Input_channel x Dim_0 x Dim_1 .. x Dim_{n-1}
  //   output: Batch_size x Output_channel x Dim_0 x Dim_1 .. x Dim_{n-1}
  // 3. NHWC_FHWC:
  //   input: Batch_size x Dim_0 x Dim_1 .. x Dim_{n-1} x Input_channel
  //   filter: Output_channel x Dim_0 x Dim_1 .. x Dim_{n-1} x Input_channel
  //   output: Batch_size x Dim_0 x Dim_1 .. x Dim_{n-1} x Output_channel
  assert(getDims().size() == filter.getDims().size());
  assert(getDims().size() > 2);
  auto dim = getDims().size() - 2;

  vector<Expr> outputIdxs = Index::boundIndexVars(getDims().size());
  // output's dim sizes will be encoded by Tensor::conv.
  // For MemRef::conv, dim sizes are implicitly encoded in the inbounds
  // checking.

  // cubeSize = Dim_0 x Dim_1 .. x Dim_{n-1} x Input_channel
  vector<Expr> cubeSize;
  switch (convLayout) {
  case ConvLayout::NHWC_HWCF: {
    for (unsigned i = 0; i < dim; i++)
      cubeSize.push_back(filter.getDim(i));
    cubeSize.push_back(filter.getDim(dim));
    break;
  }
  case ConvLayout::NCHW_FCHW: {
    for (unsigned i = 0; i < dim; i++)
      cubeSize.push_back(filter.getDim(i + 2));
    cubeSize.push_back(filter.getDim(1));
    break;
  }
  case ConvLayout::NHWC_FHWC: {
    for (unsigned i = 0; i < dim; i++)
      cubeSize.push_back(filter.getDim(i + 1));
    cubeSize.push_back(filter.getDim(dim + 1));
    break;
  }
  }
  auto cubeIdx = Index::var("cubeIdx", VarType::BOUND);
  // (Dim_0, Dim_1, Dim_{n-1}, Input_channel)
  auto cubeIdxs = from1DIdx(cubeIdx, cubeSize);
  vector<Expr> filterIdxs;
  vector<Expr> inputIdxs;

  switch (convLayout) {
  case ConvLayout::NHWC_HWCF: {
    // filterIdxs: Dim_0, Dim_1, ... Dim_{n-1}, Input_channel, Output_channel
    for (auto idx: cubeIdxs) filterIdxs.push_back(idx);
    filterIdxs.push_back(outputIdxs.back());

    // inputIdxs: Batch, Dim_0, Dim_1, ... Dim_{n-1}, Input_channel
    inputIdxs.push_back(outputIdxs.front());
    for (unsigned i = 0; i < dim; i ++)
      inputIdxs.push_back(outputIdxs[i + 1] * strides[i] +
          cubeIdxs[i] * dilations[i]);
    inputIdxs.push_back(cubeIdxs.back()); // Input_channel

    break;
  }
  case ConvLayout::NCHW_FCHW: {
    // filterIdxs: Output_channel, Input_channel, Dim_0, Dim_1, ... Dim_{n-1}
    filterIdxs.push_back(outputIdxs[1]);
    filterIdxs.push_back(cubeIdxs.back());
    for (unsigned i = 0; i < cubeIdxs.size() - 1; i++)
      filterIdxs.push_back(cubeIdxs[i]);
    
    // inputIdxs: Batch, Input_channel, Dim_0, Dim_1, ... Dim_{n-1}
    inputIdxs.push_back(outputIdxs.front());
    inputIdxs.push_back(cubeIdxs.back()); // Input_channel
    for (unsigned i = 0; i < dim; i ++)
      inputIdxs.push_back(outputIdxs[i + 2] * strides[i] +
          cubeIdxs[i] * dilations[i]);

    break;
  }
  case ConvLayout::NHWC_FHWC: {
    // filterIdxs: Output_channel, Dim_0, Dim_1, ... Dim_{n-1}, Input_channel
    filterIdxs.push_back(outputIdxs.back());
    for (auto idx: cubeIdxs) filterIdxs.push_back(idx);

    // inputIdxs: Batch, Input_channel, Dim_0, Dim_1, ... Dim_{n-1}
    inputIdxs.push_back(outputIdxs.front());
    for (unsigned i = 0; i < dim; i ++)
      inputIdxs.push_back(outputIdxs[i + 1] * strides[i] +
          cubeIdxs[i] * dilations[i]);
    inputIdxs.push_back(cubeIdxs.back()); // Input_channel

    break;
  }
  }

  Expr inputExpr = Expr::mkLambda(cubeIdx, get(inputIdxs));
  Expr filterExpr = Expr::mkLambda(cubeIdx, filter.get(filterIdxs));
  optional<Expr> initialValue = getInitValue(outputIdxs);

  Expr sz = ::get1DSize(cubeSize);
  Expr outputExpr = elemType.isa<mlir::IntegerType>() ?
    aop::intDot(inputExpr, filterExpr, sz, std::move(initialValue)) :
    aop::getFpEncoding(elemType)
      .dot(inputExpr, filterExpr, sz, std::move(initialValue));

  return {std::move(outputIdxs), std::move(outputExpr)};
}

static Sort arraySortForTensor(Sort elemSort) {
  return Sort::arraySort(Index::sort(), elemSort);
}

static Expr splatArrayForTensor(const Expr &elem) {
  return Expr::mkSplatArray(Index::sort(), elem);
}

Tensor::Tensor(mlir::Type elemType, Expr &&splat_elem, vector<Expr> &&dimvec):
    ShapedValue(elemType),
    dims(std::move(dimvec)),
    arr(splatArrayForTensor(std::move(splat_elem))),
    initialized(splatArrayForTensor(Expr::mkBool(true))) {}

// A dense tensor (1dim)
Tensor::Tensor(mlir::Type elemType, vector<Expr> &&elems1d):
    ShapedValue(elemType),
    dims({ (Expr)Index(elems1d.size()) }),
    arr(Expr::mkFreshVar(arraySortForTensor(elems1d[0].sort()), "tensor_val")),
    initialized(splatArrayForTensor(Expr::mkBool(true))) {
  for (unsigned i = 0; i < elems1d.size(); ++i)
    arr = arr.store(i, elems1d[i]);
}

Tensor::Tensor(mlir::Type elemType, vector<Expr> &&elems1d, const vector<uint64_t> &dim):
    ShapedValue(elemType),
    dims({}),
    arr(Expr::mkFreshVar(getSort(elemType), "tensor_val")),
    initialized(splatArrayForTensor(Expr::mkBool(true))) {
  for (unsigned i = 0; i < elems1d.size(); ++i)
    arr = arr.store(i, elems1d[i]);
  for (unsigned i = 0; i < dim.size(); ++i)
    dims.push_back(Index(dim[i]));
}

Tensor Tensor::fromArray(mlir::Type elemType, smt::Expr &&arr, std::vector<smt::Expr> &&dims) {
  return Tensor(elemType, std::move(dims), std::move(arr),
    splatArrayForTensor(Expr::mkBool(true)));
}

// A fresh tensor
Tensor Tensor::var(
    mlir::Type elemType, string &&name, const vector<uint64_t> &dimvec,
    bool initialized) {
  vector<Expr> e;
  for (auto i: dimvec)
    e.push_back(Index(i));
  return var(elemType, std::move(name), e, initialized);
}

Tensor Tensor::var(
    mlir::Type elemType, string &&name, const vector<Expr> &dimvec,
    bool initialized) {
  Expr arr = Expr::mkVar(
      arraySortForTensor(*convertPrimitiveTypeToSort(elemType)), std::move(name));
  Expr init = splatArrayForTensor(Expr::mkBool(initialized));
  return Tensor(elemType, vector(dimvec), std::move(arr), std::move(init));
}

// A sparse tensor.
Tensor::Tensor(
    mlir::Type elemType,
    const vector<vector<uint64_t>> &indices,
    const vector<Expr> &elems,
    const vector<uint64_t> &dims, const Expr &zero):
  ShapedValue(elemType), arr(splatArrayForTensor(zero)),
  // All elements are initialized to elems or zero.
  initialized(splatArrayForTensor(Expr::mkBool(true))) {

  assert(indices.size() == elems.size());

  for (auto d: dims)
    this->dims.push_back(Index(d));

  for (unsigned i = 0; i < indices.size(); ++i) {
    assert(indices[i].size() == dims.size());

    uint64_t ofs = indices[i][0];
    for (unsigned j = 1; j < dims.size(); ++j)
      ofs = ofs * dims[j] + indices[i][j];

    arr = arr.store(ofs, elems[i]);
  }
}

Expr Tensor::getWellDefined() const {
  Expr size = get1DSize();
  if (size.isNumeral())
    return Expr::mkBool(true);

  auto e = size.ule(MAX_TENSOR_SIZE);
  for (auto dim: dims) {
    if (dim.isNumeral()) continue;
    e = e & dim.ule(MAX_DIM_SIZE);
  }
  return e.simplify();
}

Expr Tensor::isInBounds(const vector<smt::Expr> &indices) const {
  assert(indices.size() == dims.size());

  auto inbounds = Expr::mkBool(true);
  for (unsigned i = 0; i < indices.size(); ++i)
    inbounds = inbounds & indices[i].ult(dims[i]);
  return inbounds.simplify();
}

Expr Tensor::get(const vector<Expr> &indices) const {
  return getRaw(to1DIdx(indices, dims));
}

Expr Tensor::getRaw(const Expr &indexRaw) const {
  auto e = arr.select(indexRaw);
  // Don't directly use this element!
  // Please use it with a proper wrapper (Float, Index, Integer).
  e.lockOps();
  return e;
}

Expr Tensor::isInitialized(const vector<Expr> &indices) const {
  return initialized.select(to1DIdx(indices, dims));
}

Expr Tensor::isFullyInitialized() const {
  auto vars = Index::boundIndexVars(getRank());
  return Expr::mkForall(vars, isInitialized(vars));
}

pair<Tensor, Expr> Tensor::insert(const smt::Expr &value,
    const vector<smt::Expr> &indices) const {
  auto idxvar = Index::var("idx", VarType::BOUND);
  auto cond = (Expr)idxvar == to1DIdx(indices, dims);
  auto originValue = get(from1DIdx(idxvar, dims));
  auto orgInit = isInitialized(from1DIdx(idxvar, dims));

  auto newdims = dims;
  auto newarr = Expr::mkLambda(idxvar, Expr::mkIte(cond, value, originValue));
  auto newinit = Expr::mkLambda(idxvar,
      Expr::mkIte(cond, Expr::mkBool(true), orgInit));
  return {
      {elemType, std::move(newdims), std::move(newarr), std::move(newinit)},
      isInBounds(indices)};
}

Tensor Tensor::affine(
    const vector<Expr> &newidxvars,
    vector<Expr> srcidxs,
    vector<Expr> &&newsizes) const {
  auto idxvar = Index::var("idx", VarType::BOUND);
  auto indices = from1DIdx(idxvar, newsizes);

  for (size_t i = 0; i < srcidxs.size(); ++i) {
    auto newv = srcidxs[i];
    for (size_t j = 0; j < newidxvars.size(); ++j) {
      newv = newv.substitute({ newidxvars[j] }, { indices[j] });
    }
    srcidxs[i] = newv.simplify();
  }
  auto elem = get(srcidxs);

  return {
    elemType,
    std::move(newsizes),
    Expr::mkLambda(idxvar, elem), // Value
    splatArrayForTensor(Expr::mkBool(true)) // Initialized
  };
}

Tensor Tensor::concat(const Tensor &t2, size_t axis) {
  size_t r = getRank();
  assert(r == t2.getRank() && getElemType() == t2.getElemType() && axis < r);

  auto idx = Index::boundIndexVars(r);
  auto idxForT2 = idx;
  idxForT2[axis] = idxForT2[axis] - getDim(axis);

  auto dim = getDims();
  dim[axis] = dim[axis] + t2.getDim(axis);

  auto elem = Expr::mkIte(idx[axis].ult(getDim(axis)),
        get(idx), t2.get(idxForT2));

  // UB if uninitialized elem is used
  return Tensor::mkInitializedLambda(getElemType(),
      std::move(dim), std::move(idx), std::move(elem));
}

Tensor Tensor::depthwiseConv2D(const Tensor &filter,
    const vector<Expr> &strides,
    const vector<Expr> &dilations,
    const optional<Tensor> &&bias,
    const optional<Tensor> &&output) const {

  // args should match for 2D tensors
  assert(getDims().size() == 4);
  assert(filter.getDims().size() == 4);
  assert(strides.size() == 2);
  assert(dilations.size() == 2);

  vector<Expr> outInd = bias.has_value() ? 
                          Index::boundIndexVars(4) :
                          Index::boundIndexVars(5);

  auto wDims = filter.getDims();
  auto dims = getDims();
  auto N = dims[0];
  auto C = wDims[2];
  auto M = wDims[3];
  auto n = outInd[0];
  auto c = bias.has_value() ? outInd[3].udiv(M) : outInd[3];
  auto m = bias.has_value() ? outInd[3].urem(M) : outInd[4];

  // change input to 1xHxWx1
  vector<Expr> input2DDims = {Index(1), dims[1], dims[2], Index(1)};
  vector<Expr> input2DInd = Index::boundIndexVars(4);
  Tensor input2D = Tensor::mkInitializedLambda (
                  elemType, std::move(input2DDims), std::move(input2DInd), 
                  get({n, input2DInd[1], input2DInd[2], c})
                );

  // change weight to KHxKWx1x1
  vector<Expr> weight2DDims = {wDims[0], wDims[1], Index(1), Index(1)};
  vector<Expr> weight2DInd = Index::boundIndexVars(4);
  Tensor weight2D = Tensor::mkInitializedLambda(
                  elemType, std::move(weight2DDims), std::move(weight2DInd), 
                  filter.get({weight2DInd[0], weight2DInd[1], c, m})
                );

  // change output to 1xOHxOWx1
  auto output2D = fmap(output, [&](auto unwrapped) {
    vector<Expr> output2DDims =
      {Index(1), unwrapped.getDim(1), unwrapped.getDim(2), Index(1)};
    vector<Expr> output2DInd = Index::boundIndexVars(4);
    // output2DInd[0] and output2DInd[3] are not used because the output's size
    // is 1xOHxOWx1.
    if (bias.has_value()) {
      return Tensor::mkInitializedLambda(
        elemType, std::move(output2DDims), std::move(output2DInd),
        unwrapped.get({n, output2DInd[1], output2DInd[2], outInd[3]})
      );
    } else {
      return Tensor::mkInitializedLambda(
        elemType, std::move(output2DDims), std::move(output2DInd),
        unwrapped.get({n, output2DInd[1], output2DInd[2], c, m})
      );
    }
  });

  // t2D is 1xOHxOWx1
  auto t2D = input2D.conv(weight2D, strides, dilations,
      ShapedValue::ConvLayout::NHWC_HWCF,
      std::move(output2D));

  auto t2DDims = t2D.getDims();

  auto accVal = t2D.get({Index(0), outInd[1], outInd[2], Index(0)});

  if(bias.has_value()) {
    // NxOHxOWx(C*M)
    vector<Expr> tDims = {N, t2DDims[1], t2DDims[2], C * M};

    // add bias
    auto tf = Float(accVal, elemType);
    auto biasf = Float(bias->get({outInd[3]}), elemType);

    return Tensor::mkInitializedLambda(
              elemType, std::move(tDims), std::move(outInd), 
              tf.add(biasf)
            );
  } else { 
    // NxOHxOWxCxM
    vector<Expr> tDims = {N, t2DDims[1], t2DDims[2], C, M};

    return Tensor::mkInitializedLambda(
              elemType, std::move(tDims), std::move(outInd), accVal
            );
  }
}

Tensor Tensor::conv(const Tensor &filter,
    const vector<Expr> &strides,
    const vector<Expr> &dilations,
    ConvLayout layout,
    const std::optional<Tensor> &&output) const {
  // If layout is NHWC_HWCF:
  // output[b, x[0], ..., x[N-1], k] =
  //     sum_{z[0], ..., z[N-1], q}
  //         filter[z[0], ..., z[N-1], q, k] *
  //         input[b,
  //               x[0]*strides[0] + dilation_rate[0]*z[0],
  //               ...,
  //               x[N-1]*strides[N-1] + dilation_rate[N-1]*z[N-1],
  //               q]
  // So we can calculate output dims bounds as follow. (Assuming zero based
  // index)
  // x[0]*strides[0] + dilation_rate[0]*z[0] < Original_Dim
  // x[0]*strides[0] + dilation_rate[0] * Filter_Dim - 1 < Original_Dim
  // x[0] < (Original_Dim + 1 - diltaion_rate[0] * Filter_Dim) / strides[0]
  // x[0] < ceil((Original_Dim + 1 - diltaion_rate[0] * Filter_Dim) / strides[0])
  // x[0] < (Original_Dim + 1 - diltaion_rate[0] * Filter_Dim +
  //    (strides[0] - 1)).udiv(strides[0])
  // x[0] < (Original_Dim - diltaion_rate[0] * Filter_Dim + strides[0])
  //    .udiv(strides[0])
  // Output Dim = (Original_Dim - diltaion_rate[0] * Filter_Dim +
  //    strides[0]).udiv(strides[0])
  vector<Expr> outputDims;

  switch (layout) {
  case ConvLayout::NHWC_HWCF: {
    outputDims.push_back(getDim(0)); // Input Batch Size
    for (unsigned i = 0; i < getDims().size() - 2; i ++) {
      Expr originalSize = getDim(i + 1);
      Expr filterSize = dilations[i] * filter.getDim(i);
      Expr expr = (originalSize - filterSize + strides[i]).udiv(strides[i]);
      outputDims.push_back(expr);
    }
    outputDims.push_back(filter.getDims().back()); // Output Channel
    break;
  }
  case ConvLayout::NCHW_FCHW: {
    outputDims.push_back(getDim(0)); // Input Batch Size
    outputDims.push_back(filter.getDim(0)); // Output Channel
    for (unsigned i = 0; i < getDims().size() - 2; i++) {
      Expr originalSize = getDim(i + 2);
      Expr filterSize = dilations[i] * filter.getDim(i + 2);
      Expr expr = (originalSize - filterSize + strides[i]).udiv(strides[i]);
      outputDims.push_back(expr);
    }
    break;
  }
  case ConvLayout::NHWC_FHWC: {
    outputDims.push_back(getDim(0)); // Input Batch Size
    for (unsigned i = 0; i < getDims().size() - 2; i++) {
      Expr originalSize = getDim(i + 1);
      Expr filterSize = dilations[i] * filter.getDim(i + 1);
      Expr expr = (originalSize - filterSize + strides[i]).udiv(strides[i]);
      outputDims.push_back(expr);
    }
    outputDims.push_back(filter.getDim(0)); // Output Channel
    break;
  }
  }
  auto getInitValue = [&](vector<Expr> &indices) -> optional<Expr> {
    if (output)
      return output->get(indices);
    else
      return nullopt;
  };
  auto [indices, res] = ShapedValue
      ::conv(filter, strides, dilations, layout, std::move(getInitValue));

  // UB if uninitialized elem is used
  return Tensor::mkInitializedLambda(elemType,
      std::move(outputDims), std::move(indices), std::move(res));
}

Tensor Tensor::reshape(const vector<Expr> &newdims) const {
  assert(newdims.size() > 0);
  // TODO: check whether size(newdims) == size(dims)
  return { elemType, simplifyList(newdims), Expr(arr), Expr(initialized) };
}

Tensor Tensor::matmul(const Tensor &b, bool bTransposed,
                      std::optional<Tensor> &&init) const {
  assert(dims.size() == 2);
  assert(b.dims.size() == 2);

  auto bt = bTransposed ? b : b.transpose();
  auto i = Index::var("i", VarType::BOUND);
  auto j = Index::var("j", VarType::BOUND);
  auto a_row = to1DArrayWithOfs(
      {i, Index::zero()}, {Index::one(), dims[1]});
  auto bt_row = bt.to1DArrayWithOfs(
      {j, Index::zero()}, {Index::one(), bt.dims[1]});

  auto initVal = fmap(init, [&](auto tensor) {
    return tensor.get({i, j});
  });
  auto res = elemType.isa<mlir::FloatType>() ?
    aop::getFpEncoding(elemType).dot(a_row, bt_row, dims[1], std::move(initVal)) :
    aop::intDot(a_row, bt_row, dims[1], std::move(initVal));

  // UB if uninitialized elem is used
  return mkInitializedLambda(elemType,
      {dims[0], bt.dims[0]}, {i, j}, res);
}

Tensor Tensor::elementwiseBinOp(
      const Tensor &b,
      mlir::Type resultElemType,
      const function<Expr(Expr &&e1, Expr &&e2)> &f)
      const {
  assert(getRank() == b.getRank());
  assert(elemType == b.elemType);
  // Assumed that dimension sizes are equivalent.

  auto idxvar = Index::var("idx_binop", VarType::BOUND);
  Expr elemout = f(getRaw(idxvar), b.getRaw(idxvar));

  // UB if uninitialized elem is used
  return mkLambdaFrom1D(resultElemType, getDims(), idxvar, elemout,
      /* initialized */Expr::mkBool(true));
}

Tensor Tensor::elementwiseUnaryOp(
    mlir::Type resultElemType, const function<Expr(Expr &&)> &f) const {
  auto idxvar = Index::var("idx_uop", VarType::BOUND);
  Expr elemout = f(getRaw(idxvar));

  // UB if uninitialized elem is used
  return mkLambdaFrom1D(resultElemType, getDims(), idxvar, elemout,
      /* initialized */Expr::mkBool(true));
}

Expr Tensor::dot(const Tensor &t2, optional<Expr> &&initValue) const {
  auto len = get1DSize();
  return elemType.isa<mlir::FloatType>() ?
    aop::getFpEncoding(elemType).dot(arr, t2.arr, len, std::move(initValue)) :
    aop::intDot(arr, t2.arr, len, std::move(initValue));
}

Expr Tensor::sum(Expr &&initVal) const {
  return elemType.isa<mlir::FloatType>() ?
    aop::getFpEncoding(elemType).sum(arr, get1DSize(), nullopt, std::move(initVal)) :
    aop::intSum(arr, get1DSize(), std::move(initVal));
}

Tensor Tensor::sum(unsigned axis) const {
  auto indVars = Index::boundIndexVars(getRank());

  vector<Expr> ofs; // Offsets for the 1-dim array to do summation
  ofs.insert(ofs.end(), indVars.begin(), indVars.begin() + axis);
  ofs.insert(ofs.end(), Index(0));
  ofs.insert(ofs.end(), indVars.begin() + axis + 1, indVars.end());

  vector<Expr> subtensorSz;
  vector<Expr> newSizes; // Dimension sizes of the final tensor
  for (int i = 0; i < getRank(); ++i) {
    subtensorSz.push_back(i == axis ? getDim(i) : Index(1));
    newSizes.push_back(i == axis ? Index(1) : getDim(i));
  }

  Expr row = to1DArrayWithOfs(ofs, subtensorSz);
  Expr summation = elemType.isa<mlir::FloatType>() ?
      aop::getFpEncoding(elemType).sum(row, getDim(axis)) :
      aop::intSum(row, getDim(axis));

  return Tensor::mkInitializedLambda(elemType,
      std::move(newSizes), std::move(indVars), summation);
}

Tensor Tensor::sumPool(const vector<Expr> &kernelDims,
    const vector<Expr> &strides, optional<Tensor> &&init) const {
  assert(kernelDims.size() == 2);
  assert(strides.size() == 2);

  // N, OH, OW, C
  vector<Expr> outputDims = {getDim(0),
    ((Expr)(getDim(1)+strides[0]-kernelDims[0])).udiv(strides[0]),
    ((Expr)(getDim(2)+strides[1]-kernelDims[1])).udiv(strides[1]),
    getDim(3)
  };
  vector<Expr> outputIdxs = Index::boundIndexVars(outputDims.size());
  auto initVal = fmap(init, [&](auto t) { return t.get(outputIdxs); });
  // output[N][OH][OW][C]
  //  = sum_pool(input[N][OH * stride + KH][OW * stride + KW][C])
  auto kernel1DSize = kernelDims[0] * kernelDims[1];
  auto kernelIdx = Index::var("kernelIdx", VarType::BOUND);
  auto kernelIdxs = from1DIdx(kernelIdx, kernelDims);
  vector<Expr> inputIdxs = {outputIdxs[0],
    outputIdxs[1] * strides[0] + kernelIdxs[0],
    outputIdxs[2] * strides[1] + kernelIdxs[1],
    outputIdxs[3]
  };
  auto kernelExpr = Expr::mkLambda(kernelIdx, get(inputIdxs));
  auto outputExpr = aop::getFpEncoding(elemType)
      .sum(kernelExpr, kernel1DSize, nullopt, std::move(initVal));

  return Tensor::mkInitializedLambda(elemType,
      std::move(outputDims), std::move(outputIdxs), std::move(outputExpr));
}

Tensor Tensor::avgPool(const vector<Expr> &kernelDims,
    const vector<Expr> &strides, optional<Tensor> &&init) const {
  assert(kernelDims.size() == 2);
  assert(strides.size() == 2);

  // N, OH, OW, C
  vector<Expr> outputDims = {getDim(0),
    ((Expr)(getDim(1)+strides[0]-kernelDims[0])).udiv(strides[0]),
    ((Expr)(getDim(2)+strides[1]-kernelDims[1])).udiv(strides[1]),
    getDim(3)
  };
  vector<Expr> outputIdxs = Index::boundIndexVars(outputDims.size());
  auto initVal = fmap(init, [&](auto t) { return t.get(outputIdxs); });
  // output[N][OH][OW][C]
  //  = avg_pool(input[N][OH * stride + KH][OW * stride + KW][C])
  auto kernel1DSize = kernelDims[0] * kernelDims[1];
  auto kernelIdx = Index::var("kernelIdx", VarType::BOUND);
  auto kernelIdxs = from1DIdx(kernelIdx, kernelDims);
  vector<Expr> inputIdxs = {outputIdxs[0],
    outputIdxs[1] * strides[0] + kernelIdxs[0],
    outputIdxs[2] * strides[1] + kernelIdxs[1],
    outputIdxs[3]
  };
  auto kernelExpr = Expr::mkLambda(kernelIdx, get(inputIdxs));
  auto sumExpr = aop::getFpEncoding(elemType)
      .sum(kernelExpr, kernel1DSize, nullopt, std::move(initVal));
  auto count = aop::getFpEncoding(elemType).castFromSignedInt(kernel1DSize);
  auto outputExpr = aop::getFpEncoding(elemType).div(sumExpr, count);

  return Tensor::mkInitializedLambda(elemType,
      std::move(outputDims), std::move(outputIdxs), std::move(outputExpr));
}

Tensor Tensor::maxPool(const vector<Expr> &kernelDims,
    const vector<Expr> &strides, optional<Tensor> &&init) const {
  assert(kernelDims.size() == 2);
  assert(strides.size() == 2);

  // N, OH, OW, C
  vector<Expr> outputDims = {getDim(0),
    ((Expr)(getDim(1)+strides[0]-kernelDims[0])).udiv(strides[0]),
    ((Expr)(getDim(2)+strides[1]-kernelDims[1])).udiv(strides[1]),
    getDim(3)
  };
  vector<Expr> outputIdxs = Index::boundIndexVars(outputDims.size());
  auto initVal = fmap(init, [&](auto t) { return t.get(outputIdxs); });
  // output[N][OH][OW][C]
  //  = max_pool(input[N][OH * stride + KH][OW * stride + KW][C])
  auto kernel1DSize = kernelDims[0] * kernelDims[1];
  auto kernelIdx = Index::var("kernelIdx", VarType::BOUND);
  auto kernelIdxs = from1DIdx(kernelIdx, kernelDims);
  vector<Expr> inputIdxs = {outputIdxs[0],
    outputIdxs[1] * strides[0] + kernelIdxs[0],
    outputIdxs[2] * strides[1] + kernelIdxs[1],
    outputIdxs[3]
  };
  auto kernelExpr = Expr::mkLambda(kernelIdx, get(inputIdxs));
  auto outputExpr = aop::getFpEncoding(elemType)
      .max(kernelExpr, kernel1DSize, std::move(initVal));

  return Tensor::mkInitializedLambda(elemType,
      std::move(outputDims), std::move(outputIdxs), std::move(outputExpr));
}

pair<Expr, vector<Expr>> Tensor::refines(const Tensor &other) const {
  assert(elemType == other.elemType);

  // Size mismatch check.
  // If it does, don't return index var.
  size_t sz = getDims().size();
  if (other.getDims().size() != sz)
    return {Expr::mkBool(false), {}};

  Expr size_match = Expr::mkBool(true);
  for (size_t i = 0; i < sz; ++i)
    size_match = size_match & ((Expr)other.getDim(i) == (Expr)getDim(i));
  size_match = size_match.simplify();
  if (size_match.isFalse())
    return {size_match, {}};

  // Assume that src and tgt's shape equality is already checked
  Expr i = Index::var("i", VarType::UNBOUND);
  vector<Expr> params = {i};
  ValueTy arr_i = *fromExpr(arr.select(i), elemType);
  ValueTy arr_other_i = *fromExpr(other.arr.select(i), elemType);
  auto refinement = ::refines(arr_i, arr_other_i);
  assert(refinement.second.empty());

  return {size_match &
      i.ult(::get1DSize(dims)).implies(
        initialized.select(i).implies(
            other.initialized.select(i) & refinement.first)),
    params};
}

bool Tensor::isTypeSupported(mlir::TensorType tensorTy) {
  if (!tensorTy.hasRank())
    return false;
  return convertPrimitiveTypeToSort(tensorTy.getElementType()) != nullopt;
}

Sort Tensor::getSort(mlir::Type elemType) {
  const auto elemSort = convertPrimitiveTypeToSort(elemType);
  if (!elemSort) {
    string msg;
    llvm::raw_string_ostream sstr(msg);
    elemType.print(sstr);
    sstr << "is not a valid tensor element type";
    throw UnsupportedException(std::move(msg));
  }
  return arraySortForTensor(*elemSort);
}

llvm::raw_ostream& operator<<(llvm::raw_ostream& os, const Tensor &t) {
  assert(t.dims.size() > 0);

  os << "(dim: " << or_omit(t.dims[0]);
  for (size_t i = 1; i < t.dims.size(); ++i)
    os << ", " << or_omit(t.dims[i]);
  os << ") ";

  using namespace smt::matchers;
  if (ConstSplatArray(ConstBool(false)).match(t.initialized)) {
    os << "(uninitialized)";
    return os;
  }

  const int64_t maxSizeToPrint = 16;
  int64_t dimSize;
  if (smt::get1DSize(t.dims).simplify().isInt(dimSize) &&
      dimSize <= maxSizeToPrint) {
    // Print individual elements.
    for (int64_t i = 0; i < dimSize; ++i) {
      auto idx1d = smt::simplifyList(smt::from1DIdx(Index(i), t.dims));
      vector<int64_t> idxconsts;
      for (auto &e: idx1d) {
        int64_t ii;
        bool isint = e.isInt(ii);
        assert(isint);
        (void)isint;
        idxconsts.push_back(ii);
      }
      auto elem = t.get(idx1d);
      auto init = t.isInitialized(idx1d);

      if (i != 0)
        os << ", ";
      os << "(" << idxconsts[0];
      for (size_t i = 1; i < idxconsts.size(); ++i)
        os << ", " << idxconsts[i];
      os << ") -> ";

      if (init.isTrue()) {
        auto val = fromExpr(std::move(elem), t.elemType);
        os << *val;
      } else if (init.isFalse())
        os << "(uninit.)";
      else
        os << "(unknown)";
    }
    return os;
  }

  Expr arr = t.arr;
  bool hasStore = false;
  set<uint64_t> idx1dVisited;

  while (true) {
    optional<Expr> arr2, idx, valExpr;

    if (Store(Any(arr2), Any(idx), Any(valExpr)).match(arr)) {
      uint64_t idxConst;
      bool duplicated = false;
      if (idx->isUInt(idxConst)) {
        duplicated = idx1dVisited.count(idxConst);

        if (!duplicated) {
          idx1dVisited.insert(idxConst);
          auto idxnd = from1DIdx(*idx, t.dims);
          os << "(" << *idxnd[0].simplify().asUInt();
          for (size_t i = 1; i < idxnd.size(); ++i)
            os << ", " << *idxnd[i].simplify().asUInt();
          os << ")";
        }
      } else
        os << or_omit(*idx);

      if (!duplicated)
        os << " -> " << *fromExpr(std::move(*valExpr), t.elemType) << ", ";

      arr = std::move(*arr2);
      hasStore = true;

    } else if (ConstSplatArray(Any(valExpr)).match(arr)) {
      if (hasStore)
        os << "else " << *fromExpr(std::move(*valExpr), t.elemType);
      else
        os << "a splat tensor of " << *fromExpr(std::move(*valExpr), t.elemType);
      break;

    } else {
      os << (hasStore ? "else " : "") << or_omit(arr);
      break;
    }
  }
  return os;
};

Tensor Tensor::eval(Model m) const {
  vector<Expr> dims_ev = smt::simplifyList(m.eval(dims));
  return { elemType, std::move(dims_ev),
      m.eval(arr, true).simplify(),
      m.eval(initialized, true).simplify() };
}

Tensor Tensor::reverse(unsigned axis) const {
  assert(axis < dims.size());
  auto indVars = Index::boundIndexVars(dims.size());
  auto accessIdx = indVars;
  accessIdx[axis] = dims[axis] - accessIdx[axis] - 1;

  // UB if uninitialized
  return Tensor::mkInitializedLambda(elemType, vector(dims), std::move(indVars),
      get(accessIdx));
}

Tensor Tensor::tile(const vector<unsigned> &repeat) const {
  assert(repeat.size() == dims.size());
  vector<Expr> newDims;
  for (int i = 0; i < repeat.size(); ++i)
    newDims.push_back((Expr)dims[i] * repeat[i]);

  auto indVars = Index::boundIndexVars(dims.size());
  auto accessIdx = indVars;
  for (int i = 0; i < repeat.size(); ++i)
    accessIdx[i] = accessIdx[i].urem(dims[i]);

  // UB if uninitialized
  return Tensor::mkInitializedLambda(elemType, vector(newDims), std::move(indVars),
      get(accessIdx));
}

Tensor Tensor::transpose() const {
  assert(dims.size() == 2);
  auto i = Index::var("i", VarType::BOUND);
  auto j = Index::var("j", VarType::BOUND);

  // UB if uninitialized
  return Tensor::mkInitializedLambda(
      elemType, {dims[1], dims[0]}, {j, i}, get({i, j}));
}

Tensor Tensor::mkLambda(
    mlir::Type elemType,
    vector<Expr> &&newdims, vector<Expr> &&indexvars,
    Expr body, Expr initialized) {
  if (indexvars.size() == 0) {
    // If indexvars is empty, let's assume that the tensor has only one
    // element.
    if (newdims.size() == 0) {
      newdims.push_back(Index(1));
    } else {
      [[maybe_unused]] int64_t i;
      assert(newdims.size() == 1 && newdims[0].isInt(i) && i == 1);
    }
  } else
    assert(newdims.size() == indexvars.size());

  for (auto &iv: indexvars) {
    smart_assert(iv.isVar(), "Not a variable: " << iv);
  }

  auto idx = Index::var("idx", VarType::BOUND);
  auto idxForInit = Index::var("idx_init", VarType::BOUND);
  auto idxExprs = from1DIdx(idx, newdims);
  auto idxExprsForInit = from1DIdx(idxForInit, newdims);

  if (!indexvars.empty()) {
    body = body.substitute(indexvars, idxExprs);
    initialized = initialized.substitute(indexvars, idxExprsForInit);
  }

  return { elemType, std::move(newdims),
      Expr::mkLambda(idx, body),
      Expr::mkLambda(idxForInit, initialized) };
}

Tensor Tensor::mkLambdaFrom1D(
    mlir::Type elemType,
    vector<Expr> &&newdims, Expr &&indexvar, Expr body, Expr initialized) {
  return { elemType, std::move(newdims),
      Expr::mkLambda(indexvar, body), Expr::mkLambda(indexvar, initialized) };
}

Tensor Tensor::mkInitializedLambda(
    mlir::Type elemType,
    std::vector<smt::Expr> &&newdims,
    std::vector<smt::Expr> &&indexvars,
    smt::Expr body) {
  return mkLambda(elemType, std::move(newdims), std::move(indexvars),
      std::move(body), Expr::mkBool(true));
}

Tensor Tensor::mkIte(
    function<smt::Expr(const vector<smt::Expr> &)> condFn,
    const Tensor &trueValue, const Tensor &falseValue) {
  auto trueDims = trueValue.getDims();
  auto falseDims = falseValue.getDims();
  assert(trueDims.size() == falseDims.size() &&
         trueValue.elemType == falseValue.elemType);

  auto indVars = Index::boundIndexVars(trueDims.size());
  auto isTrue = condFn(indVars) == Integer::boolTrue();

  auto retExpr = Expr::mkIte(
      isTrue, trueValue.get(indVars), falseValue.get(indVars));
  auto retInit = Expr::mkIte(
      isTrue, trueValue.isInitialized(indVars),
      falseValue.isInitialized(indVars));
  return Tensor::mkLambda(
      trueValue.elemType, std::move(trueDims), std::move(indVars),
      std::move(retExpr), std::move(retInit));
}

// attr1[i_1][i_2]..[i_N] = attr2[i_N][i_1]...[i_N-1]
// Currently support dimension = 2, 3, 4
static bool isTransposed(mlir::ElementsAttr attr1, mlir::ElementsAttr attr2) {
  auto attr1ty = attr1.getType().dyn_cast<mlir::RankedTensorType>();
  auto attr2ty = attr2.getType().dyn_cast<mlir::RankedTensorType>();
  if (!attr1ty || !attr2ty)
    return false;
  else if (attr1ty.getRank() != attr2ty.getRank())
    return false;

  if (attr1ty.getRank() == 2) {
    if (attr1ty.getDimSize(0) != attr2ty.getDimSize(1) ||
           attr1ty.getDimSize(1) != attr2ty.getDimSize(0))
      return false;

    auto attr1Values = attr1.getValues<mlir::Attribute>();
    auto attr2Values = attr2.getValues<mlir::Attribute>();
    for (uint64_t i = 0; i < attr1ty.getDimSize(0); ++i) {
      for (uint64_t j = 0; j < attr1ty.getDimSize(1); ++j) {
        if (attr1Values[{i, j}] != attr2Values[{j, i}])
          return false;
      }
    }
    return true;

  } else if (attr1ty.getRank() == 3) {
    if (attr1ty.getDimSize(0) != attr2ty.getDimSize(1) ||
          attr1ty.getDimSize(1) != attr2ty.getDimSize(2) ||
          attr1ty.getDimSize(2) != attr2ty.getDimSize(0))
      return false;

    auto attr1Values = attr1.getValues<mlir::Attribute>();
    auto attr2Values = attr2.getValues<mlir::Attribute>();
    for (uint64_t i = 0; i < attr1ty.getDimSize(0); ++i) {
      for (uint64_t j = 0; j < attr1ty.getDimSize(1); ++j) {
        for (uint64_t k = 0; k < attr1ty.getDimSize(2); ++k) {
          if (attr1Values[{i, j, k}] != attr2Values[{k, i, j}])
            return false;
        }
      }
    }
    return true;

  } else if (attr1ty.getRank() == 4) {
    if (attr1ty.getDimSize(0) != attr2ty.getDimSize(1) ||
        attr1ty.getDimSize(1) != attr2ty.getDimSize(2) ||
        attr1ty.getDimSize(2) != attr2ty.getDimSize(3) ||
        attr1ty.getDimSize(3) != attr2ty.getDimSize(0))
      return false;

    auto attr1Values = attr1.getValues<mlir::Attribute>();
    auto attr2Values = attr2.getValues<mlir::Attribute>();
    for (uint64_t i = 0; i < attr1ty.getDimSize(0); ++i) {
      for (uint64_t j = 0; j < attr1ty.getDimSize(1); ++j) {
        for (uint64_t k = 0; k < attr1ty.getDimSize(2); ++k) {
          for (uint64_t l = 0; l < attr1ty.getDimSize(3); ++l) {
            if (attr1Values[{i, j, k, l}] != attr2Values[{l, i, j, k}])
              return false;
          }
        }
      }
    }
    return true;
  } else {
    return false;
  }
}

// Currently support, <dimx1x1x1..x1xf32> -> <dimxf32>
static bool isSimpleReduction(mlir::ElementsAttr attr1, mlir::ElementsAttr attr2) {
  auto attr1ty = attr1.getType().dyn_cast<mlir::RankedTensorType>();
  auto attr2ty = attr2.getType().dyn_cast<mlir::RankedTensorType>();
  if (!attr1ty || !attr2ty)
    return false;
  if (attr1ty.getRank() <= attr2ty.getRank() || attr2ty.getRank() != 1)
    return false;
  if (attr1ty.getDimSize(0) != attr2ty.getDimSize(0))
    return false;
  for (uint64_t i = 1; i < attr1ty.getRank(); ++i)
    if (attr1ty.getDimSize(i) != 1)
      return false;

  for (uint64_t i = 0; i < attr2ty.getDimSize(0); ++i) {
    vector<uint64_t> idxs(attr1ty.getRank());
    idxs[0] = i;

    auto attr1Values = attr1.getValues<mlir::Attribute>();
    auto attr2Values = attr2.getValues<mlir::Attribute>();
    if (attr1Values[idxs] != attr2Values[i])
      return false;
  }

  return true;
}

Tensor Tensor::fromElemsAttr(mlir::RankedTensorType tensorty,
      mlir::ElementsAttr attr) {
  mlir::Type elemType = tensorty.getElementType();

  if (auto denseAttr = attr.dyn_cast<mlir::DenseElementsAttr>()) {
    if (denseAttr.isSplat()) {
      // A constant tensor's type cannot have unknown dimensions
      auto dims = ShapedValue::getDims(tensorty, false);
      auto v = attrToValueTy(denseAttr.getSplatValue<mlir::Attribute>());

      return Tensor(elemType, getExpr(v), std::move(dims));

    } else {
      int64_t rank = tensorty.getRank();
      vector<int64_t> dims;
      vector<Expr> dimExprs;
      int64_t totalSize = 1;
      for (int i = 0; i < rank; ++i) {
        auto dsize = tensorty.getDimSize(i);
        assert(dsize != mlir::ShapedType::kDynamic);
        dims.push_back(dsize);
        dimExprs.push_back(Index(dsize));
        totalSize *= dsize;
      }

      if (MAX_CONST_SIZE >= 0 && totalSize > MAX_CONST_SIZE) {
        verbose("Tensor::fromElemsAttr") << "Too many elements: " <<
            totalSize << " > " << MAX_CONST_SIZE << "\n";

        for (auto &[a, t]: abstractlyEncodedAttrs) {
          if (a == attr) {
            verbose("Tensor::fromElemsAttr") << "Returning " << (Expr)t << "\n";
            return t;

          } else if (isTransposed(attr, a)) {
            // Transposing a constant tensor happens frequently.
            verbose("Tensor::fromElemsAttr") << "Returning " << (Expr)t
                << ".affine(...)\n";
            const auto &dims = t.getDims();
            auto indVars = Index::boundIndexVars(dims.size());
            vector<Expr> newDims, newVars;

            for (uint64_t i = 1; i < dims.size(); i ++) {
              newDims.push_back(dims[i]);
              newVars.push_back(indVars[i]);
            }
            newDims.push_back(dims[0]);
            newVars.push_back(indVars[0]);

            return t.affine(newVars, indVars, std::move(newDims));
          } else if (isSimpleReduction(attr, a)) {
            // Reduction a constant tensor happens frequently.
            verbose("Tensor::fromElemsAttr") << "Returning " << (Expr)t
                << ".affine(...)\n";
            auto idx = Index::var("idx", VarType::BOUND);
            auto dims = t.getDims();
            vector<Expr> newVars = {idx};
            auto attr1ty = attr.getType().dyn_cast<mlir::RankedTensorType>();
            for (uint64_t i = 1; i < attr1ty.getRank(); ++i) {
              newVars.push_back(Index::zero());
              dims.push_back(Index::one());
            }

            return t.affine(newVars, {idx}, std::move(dims));
          }
        }

        static int count = 0;
        auto newt = Tensor::var(elemType, "unknown_const#" + to_string(count++),
            dimExprs);
        abstractlyEncodedAttrs.emplace_back(attr, newt);
        verbose("Tensor::fromElemsAttr") << "Creating a new tensor "
            << (Expr)newt << "\n";
        return newt;
      }

      // [i1, i2, ..., iN]
      vector<uint64_t> idxND(rank);
      vector<Expr> exprs;

      while (true) {
        if (idxND.back() == dims.back()) {
          int focus = rank - 1;
          while (1 <= focus && idxND[focus] == dims[focus]) {
            idxND[focus] = 0;
            idxND[focus - 1]++;
            focus--;
          }

          if (idxND[0] == dims[0])
            break;
        }

        exprs.push_back(getExpr(
            attrToValueTy(denseAttr.getValues<mlir::Attribute>()[idxND])));
        idxND.back()++;
      }

      return Tensor(elemType, std::move(exprs)).reshape(dimExprs);
    }

  } else if (auto sparseAttr = attr.dyn_cast<mlir::SparseElementsAttr>()) {
    int64_t totalSize = sparseAttr.getNumElements();
    auto sparseIndexValues = sparseAttr.getIndices().getValues<uint64_t>();
    auto elemTy = tensorty.getElementType();
    auto rank = tensorty.getRank();
    vector<uint64_t> dims;
    for (unsigned i = 0; i < rank; ++i)
      dims.push_back(tensorty.getDimSize(i));

    if (MAX_CONST_SIZE >= 0 && totalSize > MAX_CONST_SIZE) {
      verbose("Tensor::fromElemsAttr") << "Too many sparse elements: " <<
          totalSize << " > " << MAX_CONST_SIZE << "\n";

      for (auto &[a, t]: abstractlyEncodedAttrs) {
        if (a == attr) {
          verbose("Tensor::fromElemsAttr") << "Returning " << (Expr)t << "\n";
          return t;
        }
      }

      static int count = 0;
      Tensor newt = Tensor::var(elemType, "unknown_const#" + to_string(count++),
          dims);
      abstractlyEncodedAttrs.emplace_back(attr, newt);
      verbose("Tensor::fromElemsAttr") << "Creating a new tensor "
          << (Expr)newt << "\n";
      return newt;
    }

    // Unspecified locations are filled with positive zero.
    // (MLIR behaves like this)
    auto zero = getZero(elemTy);
    if (!zero)
      throw UnsupportedException("unsupported element type");

    vector<vector<uint64_t>> sparseIndices;
    vector<Expr> sparseValues;

    auto sparseIndBeg = sparseIndexValues.begin();
    while (sparseIndBeg != sparseIndexValues.end()) {
      vector<uint64_t> curIndices;
      for (unsigned i = 0; i < rank; ++i) {
        curIndices.push_back(*sparseIndBeg);
        sparseIndBeg++;
      }

      auto value = sparseAttr.getValues<mlir::Attribute>()[curIndices];
      sparseIndices.push_back(std::move(curIndices));

      auto e = attrToValueTy(value);
      sparseValues.push_back(getExpr(e));
    }
    return Tensor(elemTy, sparseIndices, sparseValues, dims, *zero);
  }

  throw UnsupportedException("unsupported attribute");
}

Expr Tensor::to1DArrayWithOfs(
      const vector<Expr> &offbegins,
      const vector<Expr> &sizes) const {
  assert(offbegins.size() == sizes.size());

  auto idxvar = Index::var("idx", VarType::BOUND);
  auto relidxs = from1DIdx(idxvar, sizes);
  vector<Expr> absidxs;
  absidxs.reserve(relidxs.size());
  for (size_t i = 0; i < relidxs.size(); ++i) {
    auto absidx = relidxs[i] + offbegins[i];
    absidxs.push_back(absidx.simplify());
  }
  auto elem = get(absidxs);
  return Expr::mkLambda(idxvar, elem);
}

MemRef::Layout::Layout(const vector<Expr> &dims):
    precondition(Expr::mkBool(true)) {
  this->indVars = Index::boundIndexVars(dims.size());
  this->inbounds = [dims](auto &indices) { return fitsInDims(indices, dims); };
  this->mapping = [dims](auto &indices) { return to1DIdx(indices, dims); };
  this->inverseMappings = [dims](auto &index) { return from1DIdx(index, dims);};
}

MemRef::Layout::Layout(const std::vector<smt::Expr> &indVars,
    const Fn &layout,
    const Fn &inbounds): indVars(indVars), inbounds(inbounds),
    precondition(Expr::mkBool(true)) // Will be replaced later
    {
  Expr condition = Expr::mkBool(true);
  vector<FnDecl> inverseFns;
  for (unsigned i = 0; i < indVars.size(); i ++) {
    auto inverseName = freshName("inverse_fn" + to_string(i));
    inverseFns.emplace_back(Index::sort(), Index::sort(), std::move(inverseName));

    condition = condition &
        (inverseFns.back()(layout(indVars)) == indVars[i]);
  }
  this->inverseMappings = [inverseFns](const Expr &idx) {
    vector<Expr> ret;
    for (auto &fn: inverseFns)
      ret.push_back(fn(idx));
    return ret;
  };

  this->mapping = layout;
  this->precondition = Expr::mkForall(indVars,
      inbounds(indVars).implies(condition));
}

MemRef::MemRef(Memory *m,
  const mlir::Type &elemTy,
  const smt::Expr &bid,
  const smt::Expr &offset,
  const std::vector<smt::Expr> &dims,
  const Layout &layout,
  const smt::Expr &isViewRef) : ShapedValue(elemTy), m(m), bid(bid),
    offset(offset), dims(dims), layout(layout), isViewRef(isViewRef) {}

MemRef::MemRef(Memory *m,
  const mlir::Type &elemty,
  const std::string &name,
  const std::vector<Expr> &dims,
  const Layout &layout):
    ShapedValue(elemty),
    m(m),
    bid(Expr::mkVar(Sort::bvSort(m->getBIDBits()), (name + "_bid").c_str())),
    offset(Index::var(name + "_offset", VarType::UNBOUND)),
    dims(dims),
    layout(layout),
    isViewRef(Expr::mkVar(Sort::boolSort(), (name + "_isviewref").c_str())) {}

MemRef::MemRef(Memory *m,
    const mlir::Type &elemty,
    const std::vector<Expr> &dims,
    const Layout &layout) :
    MemRef(m, elemty, freshName("memref"), dims, layout) {}

Expr MemRef::getPrecondition() const {
  return layout.precondition;
}

Expr MemRef::getWellDefined() const {
  Expr size = get1DSize();
  if (size.isNumeral())
    return Expr::mkBool(true);

  auto e = size.ule(MAX_MEMREF_SIZE);
  for (auto dim: dims) {
    if (dim.isNumeral()) continue;
    e = e & dim.ule(MAX_DIM_SIZE);
  }
  return e.simplify();
}

bool MemRef::isTypeSupported(mlir::MemRefType memRefTy) {
  if (!mlir::isStrided(memRefTy)) {
    // Currently we only support strided Memref.
    return {};
  }
  return convertPrimitiveTypeToSort(memRefTy.getElementType()) != nullopt;
}

MemRef::Layout MemRef::getLayout(
    mlir::MemRefType memRefTy, const vector<Expr> &dims) {
  assert(mlir::isStrided(memRefTy));

  if (memRefTy.getLayout().isIdentity())
    return MemRef::Layout(dims);

  auto getConstOrFreshVar = [](int64_t val, string &&name) -> Expr {
    return (val == mlir::ShapedType::kDynamic) ?
        Index::var(std::move(name), VarType::FRESH) : Index(val);
  };

  int64_t offset;
  llvm::SmallVector<int64_t, 4> strides;
  [[maybe_unused]] auto success =
      mlir::getStridesAndOffset(memRefTy, strides, offset);
  assert(succeeded(success) && "unexpected non-strided memref");

  Expr offsetExpr = getConstOrFreshVar(offset, "offset");
  vector<Expr> stridesExpr;
  for (size_t i = 0; i < strides.size(); i ++)
    stridesExpr.push_back(getConstOrFreshVar(strides[i], "strides"));
  auto layoutFn = [stridesExpr, offsetExpr](auto &indices) {
    Expr layout = offsetExpr;
    for (size_t i = 0; i < stridesExpr.size(); i ++)
      layout = layout + stridesExpr[i] * indices[i];
    return layout;
  };
  return MemRef::Layout(Index::boundIndexVars(strides.size()),
    layoutFn, [dims](auto &indices) { return fitsInDims(indices, dims); });
}

Expr MemRef::get(const vector<Expr> &indices) const {
  auto [idx, inbounds] = to1DIdxWithLayout(indices);
  auto loaded = m->load(elemType, bid, (Expr)offset + idx).first;
  loaded.lockOps();

  return loaded;
}

pair<Expr, AccessInfo> MemRef::getWithAccessInfo(
    const vector<Expr> &indices) const {
  auto [idx, inbounds] = to1DIdxWithLayout(indices);
  auto [loaded, info] = m->load(elemType, bid, (Expr)offset + idx);
  loaded.lockOps();

  info.inbounds &= std::move(inbounds);

  return {std::move(loaded), std::move(info)};
}

AccessInfo MemRef::store(const Expr &value,
    const std::vector<Expr> &indices) const {
  auto [idx, inbounds] = to1DIdxWithLayout(indices);
  auto info = m->store(elemType, value, bid, (Expr)offset + idx);

  info.inbounds &= std::move(inbounds);
  return info;
}

Expr MemRef::isValid1DOffset(const Expr &ofs0) const {
  Expr ofs = ofs0 - (Expr)offset;
  auto [idx, inbounds] = to1DIdxWithLayout(layout.getInverseIndices(ofs));
  return (idx == ofs) & inbounds;
}

Expr MemRef::isInBounds() const {
  auto numelem = m->getNumElementsOfMemBlock(elemType, bid);
  auto memrefSize = get1DSize();
  return numelem.uge(memrefSize) & ((Expr)offset).ule(numelem - memrefSize);
}

Expr MemRef::isGlobalBlock() const {
  return m->isGlobalBlock(elemType, bid);
}

Expr MemRef::isLocalBlock() const {
  return m->isLocalBlock(elemType, bid);
}

Expr MemRef::getLiveness() const {
  return m->getLiveness(elemType, bid);
}

Expr MemRef::isCreatedByAlloc() const {
  return m->isCreatedByAlloc(elemType, bid);
}

Expr MemRef::isFullyInitialized() const {
  auto idxs = Index::boundIndexVars(getRank());
  auto icc = getWithAccessInfo(idxs).second;
  return Expr::mkForall(idxs, icc.inbounds.implies(icc.initialized));
}

smt::Expr MemRef::noalias(const MemRef &other) const {
  if (!isIdentityMap() || !other.isIdentityMap())
    throw UnsupportedException("Noalias check with arbitrary layout memref is"
        " not supported yet");

  auto l1 = (Expr) offset;
  auto r1 = (Expr) offset + get1DSize();
  auto l2 = (Expr) other.offset;
  auto r2 = (Expr) other.offset + other.get1DSize();

  // Case 1. bid != other.bid
  // Case 2. bid == other.bid && (r2 <= l1 || r1 <= l2)
  return (!(bid == other.bid)) | ((bid == other.bid) & (r2.ule(l1) | r1.ule(l2)));
}

void MemRef::setWritable(bool writable) {
  m->setWritable(elemType, bid, writable);
}

bool MemRef::isIdentityMap() const {
  return layout.precondition.isTrue();
}

MemRef MemRef::subview(const vector<Expr> &offsets,
    const vector<Expr> &sizes,
    const vector<Expr> &strides,
    const llvm::SmallDenseSet<unsigned> &unusedDims,
    int rankDiff) {
  if (rankDiff > 0) {
    vector<Expr> indVars, reducedSizes;
    for (unsigned i = 0; i < sizes.size(); i++) {
      if (rankDiff > 0 && unusedDims.contains(i)) {
        //statically known to be 1
        indVars.push_back(Index::zero());
        rankDiff --;
      } else {
        indVars.push_back(layout.indVars[i]);
        reducedSizes.push_back(sizes[i]);
      }
    }

    auto subviewLayout = createSubViewLayout(indVars, offsets, strides, sizes);
    return MemRef(m, elemType, bid, offset, reducedSizes, subviewLayout,
        Expr::mkBool(true));
  } else {
    auto subviewLayout = createSubViewLayout(
        layout.indVars, offsets, strides, sizes);
    return MemRef(m, elemType, bid, offset, sizes, subviewLayout,
        Expr::mkBool(true));
  }
}

MemRef MemRef::reshape(const std::vector<smt::Expr> &newDims) {
  // Currently we support identity map only.
  assert (isIdentityMap());

  return MemRef(m, elemType, bid, offset,
    newDims, MemRef::Layout(newDims), Expr::mkBool(true));
}

MemRef MemRef::mkIte(smt::Expr cond,
    const MemRef &trueValue, const MemRef &falseValue) {
  auto trueDims = trueValue.getDims();
  auto falseDims = trueValue.getDims();
  assert(trueValue.m == falseValue.m);
  assert(trueDims.size() == falseDims.size() &&
         trueValue.elemType == falseValue.elemType);

  auto isTrue = (Expr) cond == Integer::boolTrue();
  auto bid = Expr::mkIte(isTrue, trueValue.bid, falseValue.bid);
  auto offset = Expr::mkIte(isTrue, trueValue.offset, falseValue.offset);
  auto isViewRef = Expr::mkIte(isTrue, trueValue.isViewRef,
      falseValue.isViewRef);
  // Assumes that trueValue.layout is equivalent to falseValue.layout.
  return MemRef(trueValue.m, trueValue.elemType,
      bid, offset, trueValue.dims, trueValue.layout, isViewRef);
}

llvm::raw_ostream& operator<<(llvm::raw_ostream& os, const MemRef &m) {
  assert(m.dims.size() > 0);
  os << "(bid: " << or_omit(m.bid)
    << ", offset: " << or_omit(m.offset)
    << ", dim: " << or_omit(m.dims[0]);
  for (size_t i = 1; i < m.dims.size(); ++i)
    os << ", " << or_omit(m.dims[i]);
  os << ")";
  return os;
};

std::pair<Expr, vector<Expr>> MemRef::refines(const MemRef &other) const {
  return {(Expr) other == (Expr) *this, {}};
}

MemRef MemRef::eval(Model mdl) const {
  MemRef m2 = *this;
  for (size_t i = 0; i < m2.dims.size(); ++i)
    m2.dims[i] = mdl.eval(m2.dims[i], true).simplify();

  m2.bid = mdl.eval(m2.bid, true).simplify();
  m2.offset = mdl.eval(m2.offset, true).simplify();

  return m2;
}

pair<Expr, Expr> MemRef::to1DIdxWithLayout(const vector<Expr> &_idxs) const {
  auto idxs = _idxs;
  if (idxs.empty()) {
    idxs.push_back(Index::zero());
  }
  auto Expr = layout.mapping(idxs);
  auto inbounds = layout.inbounds(idxs);
  return {Expr, inbounds};
}

MemRef::Layout MemRef::createSubViewLayout(
    const vector<Expr> &indVarsOrZero,
    const vector<Expr> &offsets,
    const vector<Expr> &strides,
    const vector<Expr> &sizes) {
  // Before : <(d0, d1) -> (d0 * s0 + d1)>,
  // After: <(d0, d1) ->
  //    ((indVars[0] * strides[0] + offsets[0]) * s0 +
  //      indVars[1] * strides[1] + offsets[1])>
  // indVars[i] can be Index::zero() if the dimension is reduced.
  assert(layout.indVars.size() == indVarsOrZero.size());
  assert(layout.indVars.size() == offsets.size());
  assert(layout.indVars.size() == strides.size());
  assert(layout.indVars.size() == sizes.size());
  unsigned numVarsBefore = indVarsOrZero.size();

  vector<Expr> indVars;
  vector<unsigned> zeroOffsets;
  for (unsigned i = 0; i < numVarsBefore; ++i) {
    if (!indVarsOrZero[i].isVar()) {
      uint64_t u;
      smart_assert(indVarsOrZero[i].isUInt(u) && u == 0,
          "Must be either var or constant 0, but for " << i << "'th element "
          "we got " << indVarsOrZero[i]);
      zeroOffsets.push_back(i);
    } else {
      indVars.push_back(indVarsOrZero[i]);
    }
  }

  auto insertZeros = [zeroOffsets, numVarsBefore](
      const vector<Expr> &indVars) -> vector<Expr> {
    assert(indVars.size() + zeroOffsets.size() == numVarsBefore);

    vector<Expr> indVarsOrZero = indVars;
    for (auto ofs: zeroOffsets)
      indVarsOrZero.insert(indVarsOrZero.begin() + ofs, Index(0));
    return indVarsOrZero;
  };
  auto transformIndices = [strides, offsets](const vector<Expr> &indices)
      -> vector<Expr> {
    vector<Expr> indices2;
    for (unsigned i = 0; i < strides.size(); ++i)
      indices2.push_back(indices[i] * strides[i] + offsets[i]);
    return indices2;
  };

  auto oldLayout = this->layout;
  auto transformedInbounds = [=](const vector<Expr> &idxs) {
    auto idxsOrZero = insertZeros(idxs);
    auto originalIndices = transformIndices(idxsOrZero);
    return oldLayout.inbounds(originalIndices) & fitsInDims(idxsOrZero, sizes);
  };

  auto transformedLayout = [=](const vector<Expr> &idxs) -> Expr {
    auto idxsOrZero = insertZeros(idxs);
    auto originalIndices = transformIndices(idxsOrZero);
    return oldLayout.mapping(originalIndices);
  };

  return Layout(indVars, transformedLayout, transformedInbounds);
}

vector<Expr> MemRef::Layout::getInverseIndices(const Expr &idx) const {
  return inverseMappings(idx);
}

llvm::raw_ostream& operator<<(llvm::raw_ostream &os, const ValueTy &v) {
  visit([&](auto &&itm) {
    os << itm;
  }, v);
  return os;
}

Expr getExpr(const ValueTy &v) {
  optional<Expr> e;
  visit([&](auto &&itm) {
    e = (Expr)itm;
  }, v);
  return std::move(*e);
}

ValueTy eval(const ValueTy &v, smt::Model m) {
  optional<ValueTy> e;
  visit([&](auto &&itm) {
    e = itm.eval(m);
  }, v);
  return std::move(*e);
}

ValueTy attrToValueTy(mlir::Attribute a) {
  if (auto fty = a.dyn_cast<mlir::FloatAttr>()) {
    return Float::constant(fty.getValue(), fty.getType());
  } else if (auto ity = a.dyn_cast<mlir::IntegerAttr>()) {
    if (ity.getType().isIndex()) {
      llvm::APInt i = a.dyn_cast<mlir::IntegerAttr>().getValue();
      assert(i.getBitWidth() == 64);
      int64_t ii = i.getSExtValue();
      assert(-2147483648ll <= ii && ii <= 2147483647ll);
      return Index(ii);
    }
    if (64 < ity.getType().getIntOrFloatBitWidth())
      throw UnsupportedException("Integer size is too large");

    return Integer(a.dyn_cast<mlir::IntegerAttr>().getValue());
  }

  throw UnsupportedException("Unsupported type");
}

optional<ValueTy> fromExpr(Expr &&e, mlir::Type ty) {
  if (ty.isIndex())
    return Index(e);
  else if (ty.isa<mlir::FloatType>())
    return Float(e, ty);
  else if (ty.isa<mlir::IntegerType>()) {
    assert(e.sort().bitwidth() == ty.getIntOrFloatBitWidth());
    return Integer(e);
  }
  return {};
}

pair<Expr, vector<Expr>> refines(const ValueTy &v_tgt, const ValueTy &v_src) {
  optional<Expr> refines_opt;
  vector<Expr> params;

  visit([&](auto &&src, auto &&tgt) {
    auto typedSrc = (decltype(tgt)) src;
    tie(refines_opt, params) = tgt.refines(typedSrc);
  }, v_tgt, v_src);

  return {std::move(*refines_opt), std::move(params)};
}
