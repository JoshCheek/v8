// Copyright 2014 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/base/adapters.h"
#include "src/compiler/instruction-selector-impl.h"
#include "src/compiler/node-matchers.h"
#include "src/compiler/node-properties.h"

namespace v8 {
namespace internal {
namespace compiler {

// Adds X87-specific methods for generating operands.
class X87OperandGenerator final : public OperandGenerator {
 public:
  explicit X87OperandGenerator(InstructionSelector* selector)
      : OperandGenerator(selector) {}

  InstructionOperand UseByteRegister(Node* node) {
    // TODO(titzer): encode byte register use constraints.
    return UseFixed(node, edx);
  }

  InstructionOperand DefineAsByteRegister(Node* node) {
    // TODO(titzer): encode byte register def constraints.
    return DefineAsRegister(node);
  }

  bool CanBeMemoryOperand(InstructionCode opcode, Node* node, Node* input) {
    if (input->opcode() != IrOpcode::kLoad ||
        !selector()->CanCover(node, input)) {
      return false;
    }
    MachineRepresentation rep =
        LoadRepresentationOf(input->op()).representation();
    switch (opcode) {
      case kX87Cmp:
      case kX87Test:
        return rep == MachineRepresentation::kWord32 ||
               rep == MachineRepresentation::kTagged;
      case kX87Cmp16:
      case kX87Test16:
        return rep == MachineRepresentation::kWord16;
      case kX87Cmp8:
      case kX87Test8:
        return rep == MachineRepresentation::kWord8;
      default:
        break;
    }
    return false;
  }

  InstructionOperand CreateImmediate(int imm) {
    return sequence()->AddImmediate(Constant(imm));
  }

  bool CanBeImmediate(Node* node) {
    switch (node->opcode()) {
      case IrOpcode::kInt32Constant:
      case IrOpcode::kNumberConstant:
      case IrOpcode::kExternalConstant:
      case IrOpcode::kRelocatableInt32Constant:
      case IrOpcode::kRelocatableInt64Constant:
        return true;
      case IrOpcode::kHeapConstant: {
// TODO(bmeurer): We must not dereference handles concurrently. If we
// really have to this here, then we need to find a way to put this
// information on the HeapConstant node already.
#if 0
        // Constants in new space cannot be used as immediates in V8 because
        // the GC does not scan code objects when collecting the new generation.
        Handle<HeapObject> value = OpParameter<Handle<HeapObject>>(node);
        Isolate* isolate = value->GetIsolate();
        return !isolate->heap()->InNewSpace(*value);
#endif
      }
      default:
        return false;
    }
  }

  AddressingMode GenerateMemoryOperandInputs(Node* index, int scale, Node* base,
                                             Node* displacement_node,
                                             InstructionOperand inputs[],
                                             size_t* input_count) {
    AddressingMode mode = kMode_MRI;
    int32_t displacement = (displacement_node == nullptr)
                               ? 0
                               : OpParameter<int32_t>(displacement_node);
    if (base != nullptr) {
      if (base->opcode() == IrOpcode::kInt32Constant) {
        displacement += OpParameter<int32_t>(base);
        base = nullptr;
      }
    }
    if (base != nullptr) {
      inputs[(*input_count)++] = UseRegister(base);
      if (index != nullptr) {
        DCHECK(scale >= 0 && scale <= 3);
        inputs[(*input_count)++] = UseRegister(index);
        if (displacement != 0) {
          inputs[(*input_count)++] = TempImmediate(displacement);
          static const AddressingMode kMRnI_modes[] = {kMode_MR1I, kMode_MR2I,
                                                       kMode_MR4I, kMode_MR8I};
          mode = kMRnI_modes[scale];
        } else {
          static const AddressingMode kMRn_modes[] = {kMode_MR1, kMode_MR2,
                                                      kMode_MR4, kMode_MR8};
          mode = kMRn_modes[scale];
        }
      } else {
        if (displacement == 0) {
          mode = kMode_MR;
        } else {
          inputs[(*input_count)++] = TempImmediate(displacement);
          mode = kMode_MRI;
        }
      }
    } else {
      DCHECK(scale >= 0 && scale <= 3);
      if (index != nullptr) {
        inputs[(*input_count)++] = UseRegister(index);
        if (displacement != 0) {
          inputs[(*input_count)++] = TempImmediate(displacement);
          static const AddressingMode kMnI_modes[] = {kMode_MRI, kMode_M2I,
                                                      kMode_M4I, kMode_M8I};
          mode = kMnI_modes[scale];
        } else {
          static const AddressingMode kMn_modes[] = {kMode_MR, kMode_M2,
                                                     kMode_M4, kMode_M8};
          mode = kMn_modes[scale];
        }
      } else {
        inputs[(*input_count)++] = TempImmediate(displacement);
        return kMode_MI;
      }
    }
    return mode;
  }

  AddressingMode GetEffectiveAddressMemoryOperand(Node* node,
                                                  InstructionOperand inputs[],
                                                  size_t* input_count) {
    BaseWithIndexAndDisplacement32Matcher m(node, true);
    DCHECK(m.matches());
    if ((m.displacement() == nullptr || CanBeImmediate(m.displacement()))) {
      return GenerateMemoryOperandInputs(m.index(), m.scale(), m.base(),
                                         m.displacement(), inputs, input_count);
    } else {
      inputs[(*input_count)++] = UseRegister(node->InputAt(0));
      inputs[(*input_count)++] = UseRegister(node->InputAt(1));
      return kMode_MR1;
    }
  }

  bool CanBeBetterLeftOperand(Node* node) const {
    return !selector()->IsLive(node);
  }
};


void InstructionSelector::VisitLoad(Node* node) {
  LoadRepresentation load_rep = LoadRepresentationOf(node->op());

  ArchOpcode opcode = kArchNop;
  switch (load_rep.representation()) {
    case MachineRepresentation::kFloat32:
      opcode = kX87Movss;
      break;
    case MachineRepresentation::kFloat64:
      opcode = kX87Movsd;
      break;
    case MachineRepresentation::kBit:  // Fall through.
    case MachineRepresentation::kWord8:
      opcode = load_rep.IsSigned() ? kX87Movsxbl : kX87Movzxbl;
      break;
    case MachineRepresentation::kWord16:
      opcode = load_rep.IsSigned() ? kX87Movsxwl : kX87Movzxwl;
      break;
    case MachineRepresentation::kTagged:  // Fall through.
    case MachineRepresentation::kWord32:
      opcode = kX87Movl;
      break;
    case MachineRepresentation::kWord64:   // Fall through.
    case MachineRepresentation::kSimd128:  // Fall through.
    case MachineRepresentation::kNone:
      UNREACHABLE();
      return;
  }

  X87OperandGenerator g(this);
  InstructionOperand outputs[1];
  outputs[0] = g.DefineAsRegister(node);
  InstructionOperand inputs[3];
  size_t input_count = 0;
  AddressingMode mode =
      g.GetEffectiveAddressMemoryOperand(node, inputs, &input_count);
  InstructionCode code = opcode | AddressingModeField::encode(mode);
  Emit(code, 1, outputs, input_count, inputs);
}


void InstructionSelector::VisitStore(Node* node) {
  X87OperandGenerator g(this);
  Node* base = node->InputAt(0);
  Node* index = node->InputAt(1);
  Node* value = node->InputAt(2);

  StoreRepresentation store_rep = StoreRepresentationOf(node->op());
  WriteBarrierKind write_barrier_kind = store_rep.write_barrier_kind();
  MachineRepresentation rep = store_rep.representation();

  if (write_barrier_kind != kNoWriteBarrier) {
    DCHECK_EQ(MachineRepresentation::kTagged, rep);
    AddressingMode addressing_mode;
    InstructionOperand inputs[3];
    size_t input_count = 0;
    inputs[input_count++] = g.UseUniqueRegister(base);
    if (g.CanBeImmediate(index)) {
      inputs[input_count++] = g.UseImmediate(index);
      addressing_mode = kMode_MRI;
    } else {
      inputs[input_count++] = g.UseUniqueRegister(index);
      addressing_mode = kMode_MR1;
    }
    inputs[input_count++] = g.UseUniqueRegister(value);
    RecordWriteMode record_write_mode = RecordWriteMode::kValueIsAny;
    switch (write_barrier_kind) {
      case kNoWriteBarrier:
        UNREACHABLE();
        break;
      case kMapWriteBarrier:
        record_write_mode = RecordWriteMode::kValueIsMap;
        break;
      case kPointerWriteBarrier:
        record_write_mode = RecordWriteMode::kValueIsPointer;
        break;
      case kFullWriteBarrier:
        record_write_mode = RecordWriteMode::kValueIsAny;
        break;
    }
    InstructionOperand temps[] = {g.TempRegister(), g.TempRegister()};
    size_t const temp_count = arraysize(temps);
    InstructionCode code = kArchStoreWithWriteBarrier;
    code |= AddressingModeField::encode(addressing_mode);
    code |= MiscField::encode(static_cast<int>(record_write_mode));
    Emit(code, 0, nullptr, input_count, inputs, temp_count, temps);
  } else {
    ArchOpcode opcode = kArchNop;
    switch (rep) {
      case MachineRepresentation::kFloat32:
        opcode = kX87Movss;
        break;
      case MachineRepresentation::kFloat64:
        opcode = kX87Movsd;
        break;
      case MachineRepresentation::kBit:  // Fall through.
      case MachineRepresentation::kWord8:
        opcode = kX87Movb;
        break;
      case MachineRepresentation::kWord16:
        opcode = kX87Movw;
        break;
      case MachineRepresentation::kTagged:  // Fall through.
      case MachineRepresentation::kWord32:
        opcode = kX87Movl;
        break;
      case MachineRepresentation::kWord64:   // Fall through.
      case MachineRepresentation::kSimd128:  // Fall through.
      case MachineRepresentation::kNone:
        UNREACHABLE();
        return;
    }

    InstructionOperand val;
    if (g.CanBeImmediate(value)) {
      val = g.UseImmediate(value);
    } else if (rep == MachineRepresentation::kWord8 ||
               rep == MachineRepresentation::kBit) {
      val = g.UseByteRegister(value);
    } else {
      val = g.UseRegister(value);
    }

    InstructionOperand inputs[4];
    size_t input_count = 0;
    AddressingMode addressing_mode =
        g.GetEffectiveAddressMemoryOperand(node, inputs, &input_count);
    InstructionCode code =
        opcode | AddressingModeField::encode(addressing_mode);
    inputs[input_count++] = val;
    Emit(code, 0, static_cast<InstructionOperand*>(nullptr), input_count,
         inputs);
  }
}


void InstructionSelector::VisitCheckedLoad(Node* node) {
  CheckedLoadRepresentation load_rep = CheckedLoadRepresentationOf(node->op());
  X87OperandGenerator g(this);
  Node* const buffer = node->InputAt(0);
  Node* const offset = node->InputAt(1);
  Node* const length = node->InputAt(2);
  ArchOpcode opcode = kArchNop;
  switch (load_rep.representation()) {
    case MachineRepresentation::kWord8:
      opcode = load_rep.IsSigned() ? kCheckedLoadInt8 : kCheckedLoadUint8;
      break;
    case MachineRepresentation::kWord16:
      opcode = load_rep.IsSigned() ? kCheckedLoadInt16 : kCheckedLoadUint16;
      break;
    case MachineRepresentation::kWord32:
      opcode = kCheckedLoadWord32;
      break;
    case MachineRepresentation::kFloat32:
      opcode = kCheckedLoadFloat32;
      break;
    case MachineRepresentation::kFloat64:
      opcode = kCheckedLoadFloat64;
      break;
    case MachineRepresentation::kBit:      // Fall through.
    case MachineRepresentation::kTagged:   // Fall through.
    case MachineRepresentation::kWord64:   // Fall through.
    case MachineRepresentation::kSimd128:  // Fall through.
    case MachineRepresentation::kNone:
      UNREACHABLE();
      return;
  }
  InstructionOperand offset_operand = g.UseRegister(offset);
  InstructionOperand length_operand =
      g.CanBeImmediate(length) ? g.UseImmediate(length) : g.UseRegister(length);
  if (g.CanBeImmediate(buffer)) {
    Emit(opcode | AddressingModeField::encode(kMode_MRI),
         g.DefineAsRegister(node), offset_operand, length_operand,
         offset_operand, g.UseImmediate(buffer));
  } else {
    Emit(opcode | AddressingModeField::encode(kMode_MR1),
         g.DefineAsRegister(node), offset_operand, length_operand,
         g.UseRegister(buffer), offset_operand);
  }
}


void InstructionSelector::VisitCheckedStore(Node* node) {
  MachineRepresentation rep = CheckedStoreRepresentationOf(node->op());
  X87OperandGenerator g(this);
  Node* const buffer = node->InputAt(0);
  Node* const offset = node->InputAt(1);
  Node* const length = node->InputAt(2);
  Node* const value = node->InputAt(3);
  ArchOpcode opcode = kArchNop;
  switch (rep) {
    case MachineRepresentation::kWord8:
      opcode = kCheckedStoreWord8;
      break;
    case MachineRepresentation::kWord16:
      opcode = kCheckedStoreWord16;
      break;
    case MachineRepresentation::kWord32:
      opcode = kCheckedStoreWord32;
      break;
    case MachineRepresentation::kFloat32:
      opcode = kCheckedStoreFloat32;
      break;
    case MachineRepresentation::kFloat64:
      opcode = kCheckedStoreFloat64;
      break;
    case MachineRepresentation::kBit:      // Fall through.
    case MachineRepresentation::kTagged:   // Fall through.
    case MachineRepresentation::kWord64:   // Fall through.
    case MachineRepresentation::kSimd128:  // Fall through.
    case MachineRepresentation::kNone:
      UNREACHABLE();
      return;
  }
  InstructionOperand value_operand =
      g.CanBeImmediate(value) ? g.UseImmediate(value)
                              : ((rep == MachineRepresentation::kWord8 ||
                                  rep == MachineRepresentation::kBit)
                                     ? g.UseByteRegister(value)
                                     : g.UseRegister(value));
  InstructionOperand offset_operand = g.UseRegister(offset);
  InstructionOperand length_operand =
      g.CanBeImmediate(length) ? g.UseImmediate(length) : g.UseRegister(length);
  if (g.CanBeImmediate(buffer)) {
    Emit(opcode | AddressingModeField::encode(kMode_MRI), g.NoOutput(),
         offset_operand, length_operand, value_operand, offset_operand,
         g.UseImmediate(buffer));
  } else {
    Emit(opcode | AddressingModeField::encode(kMode_MR1), g.NoOutput(),
         offset_operand, length_operand, value_operand, g.UseRegister(buffer),
         offset_operand);
  }
}

namespace {

// Shared routine for multiple binary operations.
void VisitBinop(InstructionSelector* selector, Node* node,
                InstructionCode opcode, FlagsContinuation* cont) {
  X87OperandGenerator g(selector);
  Int32BinopMatcher m(node);
  Node* left = m.left().node();
  Node* right = m.right().node();
  InstructionOperand inputs[4];
  size_t input_count = 0;
  InstructionOperand outputs[2];
  size_t output_count = 0;

  // TODO(turbofan): match complex addressing modes.
  if (left == right) {
    // If both inputs refer to the same operand, enforce allocating a register
    // for both of them to ensure that we don't end up generating code like
    // this:
    //
    //   mov eax, [ebp-0x10]
    //   add eax, [ebp-0x10]
    //   jo label
    InstructionOperand const input = g.UseRegister(left);
    inputs[input_count++] = input;
    inputs[input_count++] = input;
  } else if (g.CanBeImmediate(right)) {
    inputs[input_count++] = g.UseRegister(left);
    inputs[input_count++] = g.UseImmediate(right);
  } else {
    if (node->op()->HasProperty(Operator::kCommutative) &&
        g.CanBeBetterLeftOperand(right)) {
      std::swap(left, right);
    }
    inputs[input_count++] = g.UseRegister(left);
    inputs[input_count++] = g.Use(right);
  }

  if (cont->IsBranch()) {
    inputs[input_count++] = g.Label(cont->true_block());
    inputs[input_count++] = g.Label(cont->false_block());
  }

  outputs[output_count++] = g.DefineSameAsFirst(node);
  if (cont->IsSet()) {
    outputs[output_count++] = g.DefineAsRegister(cont->result());
  }

  DCHECK_NE(0u, input_count);
  DCHECK_NE(0u, output_count);
  DCHECK_GE(arraysize(inputs), input_count);
  DCHECK_GE(arraysize(outputs), output_count);

  opcode = cont->Encode(opcode);
  if (cont->IsDeoptimize()) {
    selector->EmitDeoptimize(opcode, output_count, outputs, input_count, inputs,
                             cont->frame_state());
  } else {
    selector->Emit(opcode, output_count, outputs, input_count, inputs);
  }
}


// Shared routine for multiple binary operations.
void VisitBinop(InstructionSelector* selector, Node* node,
                InstructionCode opcode) {
  FlagsContinuation cont;
  VisitBinop(selector, node, opcode, &cont);
}

}  // namespace

void InstructionSelector::VisitWord32And(Node* node) {
  VisitBinop(this, node, kX87And);
}


void InstructionSelector::VisitWord32Or(Node* node) {
  VisitBinop(this, node, kX87Or);
}


void InstructionSelector::VisitWord32Xor(Node* node) {
  X87OperandGenerator g(this);
  Int32BinopMatcher m(node);
  if (m.right().Is(-1)) {
    Emit(kX87Not, g.DefineSameAsFirst(node), g.UseRegister(m.left().node()));
  } else {
    VisitBinop(this, node, kX87Xor);
  }
}


// Shared routine for multiple shift operations.
static inline void VisitShift(InstructionSelector* selector, Node* node,
                              ArchOpcode opcode) {
  X87OperandGenerator g(selector);
  Node* left = node->InputAt(0);
  Node* right = node->InputAt(1);

  if (g.CanBeImmediate(right)) {
    selector->Emit(opcode, g.DefineSameAsFirst(node), g.UseRegister(left),
                   g.UseImmediate(right));
  } else {
    selector->Emit(opcode, g.DefineSameAsFirst(node), g.UseRegister(left),
                   g.UseFixed(right, ecx));
  }
}


namespace {

void VisitMulHigh(InstructionSelector* selector, Node* node,
                  ArchOpcode opcode) {
  X87OperandGenerator g(selector);
  InstructionOperand temps[] = {g.TempRegister(eax)};
  selector->Emit(
      opcode, g.DefineAsFixed(node, edx), g.UseFixed(node->InputAt(0), eax),
      g.UseUniqueRegister(node->InputAt(1)), arraysize(temps), temps);
}


void VisitDiv(InstructionSelector* selector, Node* node, ArchOpcode opcode) {
  X87OperandGenerator g(selector);
  InstructionOperand temps[] = {g.TempRegister(edx)};
  selector->Emit(opcode, g.DefineAsFixed(node, eax),
                 g.UseFixed(node->InputAt(0), eax),
                 g.UseUnique(node->InputAt(1)), arraysize(temps), temps);
}


void VisitMod(InstructionSelector* selector, Node* node, ArchOpcode opcode) {
  X87OperandGenerator g(selector);
  InstructionOperand temps[] = {g.TempRegister(eax)};
  selector->Emit(opcode, g.DefineAsFixed(node, edx),
                 g.UseFixed(node->InputAt(0), eax),
                 g.UseUnique(node->InputAt(1)), arraysize(temps), temps);
}

void EmitLea(InstructionSelector* selector, Node* result, Node* index,
             int scale, Node* base, Node* displacement) {
  X87OperandGenerator g(selector);
  InstructionOperand inputs[4];
  size_t input_count = 0;
  AddressingMode mode = g.GenerateMemoryOperandInputs(
      index, scale, base, displacement, inputs, &input_count);

  DCHECK_NE(0u, input_count);
  DCHECK_GE(arraysize(inputs), input_count);

  InstructionOperand outputs[1];
  outputs[0] = g.DefineAsRegister(result);

  InstructionCode opcode = AddressingModeField::encode(mode) | kX87Lea;

  selector->Emit(opcode, 1, outputs, input_count, inputs);
}

}  // namespace


void InstructionSelector::VisitWord32Shl(Node* node) {
  Int32ScaleMatcher m(node, true);
  if (m.matches()) {
    Node* index = node->InputAt(0);
    Node* base = m.power_of_two_plus_one() ? index : nullptr;
    EmitLea(this, node, index, m.scale(), base, nullptr);
    return;
  }
  VisitShift(this, node, kX87Shl);
}


void InstructionSelector::VisitWord32Shr(Node* node) {
  VisitShift(this, node, kX87Shr);
}


void InstructionSelector::VisitWord32Sar(Node* node) {
  VisitShift(this, node, kX87Sar);
}

void InstructionSelector::VisitInt32PairAdd(Node* node) {
  X87OperandGenerator g(this);

  // We use UseUniqueRegister here to avoid register sharing with the temp
  // register.
  InstructionOperand inputs[] = {
      g.UseRegister(node->InputAt(0)), g.UseUniqueRegister(node->InputAt(1)),
      g.UseRegister(node->InputAt(2)), g.UseUniqueRegister(node->InputAt(3))};

  InstructionOperand outputs[] = {
      g.DefineSameAsFirst(node),
      g.DefineAsRegister(NodeProperties::FindProjection(node, 1))};

  InstructionOperand temps[] = {g.TempRegister()};

  Emit(kX87AddPair, 2, outputs, 4, inputs, 1, temps);
}

void InstructionSelector::VisitInt32PairSub(Node* node) {
  X87OperandGenerator g(this);

  // We use UseUniqueRegister here to avoid register sharing with the temp
  // register.
  InstructionOperand inputs[] = {
      g.UseRegister(node->InputAt(0)), g.UseUniqueRegister(node->InputAt(1)),
      g.UseRegister(node->InputAt(2)), g.UseUniqueRegister(node->InputAt(3))};

  InstructionOperand outputs[] = {
      g.DefineSameAsFirst(node),
      g.DefineAsRegister(NodeProperties::FindProjection(node, 1))};

  InstructionOperand temps[] = {g.TempRegister()};

  Emit(kX87SubPair, 2, outputs, 4, inputs, 1, temps);
}

void InstructionSelector::VisitInt32PairMul(Node* node) {
  X87OperandGenerator g(this);

  // InputAt(3) explicitly shares ecx with OutputRegister(1) to save one
  // register and one mov instruction.
  InstructionOperand inputs[] = {
      g.UseUnique(node->InputAt(0)), g.UseUnique(node->InputAt(1)),
      g.UseUniqueRegister(node->InputAt(2)), g.UseFixed(node->InputAt(3), ecx)};

  InstructionOperand outputs[] = {
      g.DefineAsFixed(node, eax),
      g.DefineAsFixed(NodeProperties::FindProjection(node, 1), ecx)};

  InstructionOperand temps[] = {g.TempRegister(edx)};

  Emit(kX87MulPair, 2, outputs, 4, inputs, 1, temps);
}

void VisitWord32PairShift(InstructionSelector* selector, InstructionCode opcode,
                          Node* node) {
  X87OperandGenerator g(selector);

  Node* shift = node->InputAt(2);
  InstructionOperand shift_operand;
  if (g.CanBeImmediate(shift)) {
    shift_operand = g.UseImmediate(shift);
  } else {
    shift_operand = g.UseFixed(shift, ecx);
  }
  InstructionOperand inputs[] = {g.UseFixed(node->InputAt(0), eax),
                                 g.UseFixed(node->InputAt(1), edx),
                                 shift_operand};

  InstructionOperand outputs[] = {
      g.DefineAsFixed(node, eax),
      g.DefineAsFixed(NodeProperties::FindProjection(node, 1), edx)};

  selector->Emit(opcode, 2, outputs, 3, inputs);
}

void InstructionSelector::VisitWord32PairShl(Node* node) {
  VisitWord32PairShift(this, kX87ShlPair, node);
}

void InstructionSelector::VisitWord32PairShr(Node* node) {
  VisitWord32PairShift(this, kX87ShrPair, node);
}

void InstructionSelector::VisitWord32PairSar(Node* node) {
  VisitWord32PairShift(this, kX87SarPair, node);
}

void InstructionSelector::VisitWord32Ror(Node* node) {
  VisitShift(this, node, kX87Ror);
}


void InstructionSelector::VisitWord32Clz(Node* node) {
  X87OperandGenerator g(this);
  Emit(kX87Lzcnt, g.DefineAsRegister(node), g.Use(node->InputAt(0)));
}


void InstructionSelector::VisitWord32Ctz(Node* node) { UNREACHABLE(); }


void InstructionSelector::VisitWord32ReverseBits(Node* node) { UNREACHABLE(); }


void InstructionSelector::VisitWord32Popcnt(Node* node) {
  X87OperandGenerator g(this);
  Emit(kX87Popcnt, g.DefineAsRegister(node), g.Use(node->InputAt(0)));
}


void InstructionSelector::VisitInt32Add(Node* node) {
  X87OperandGenerator g(this);

  // Try to match the Add to a lea pattern
  BaseWithIndexAndDisplacement32Matcher m(node);
  if (m.matches() &&
      (m.displacement() == nullptr || g.CanBeImmediate(m.displacement()))) {
    InstructionOperand inputs[4];
    size_t input_count = 0;
    AddressingMode mode = g.GenerateMemoryOperandInputs(
        m.index(), m.scale(), m.base(), m.displacement(), inputs, &input_count);

    DCHECK_NE(0u, input_count);
    DCHECK_GE(arraysize(inputs), input_count);

    InstructionOperand outputs[1];
    outputs[0] = g.DefineAsRegister(node);

    InstructionCode opcode = AddressingModeField::encode(mode) | kX87Lea;
    Emit(opcode, 1, outputs, input_count, inputs);
    return;
  }

  // No lea pattern match, use add
  VisitBinop(this, node, kX87Add);
}


void InstructionSelector::VisitInt32Sub(Node* node) {
  X87OperandGenerator g(this);
  Int32BinopMatcher m(node);
  if (m.left().Is(0)) {
    Emit(kX87Neg, g.DefineSameAsFirst(node), g.Use(m.right().node()));
  } else {
    VisitBinop(this, node, kX87Sub);
  }
}


void InstructionSelector::VisitInt32Mul(Node* node) {
  Int32ScaleMatcher m(node, true);
  if (m.matches()) {
    Node* index = node->InputAt(0);
    Node* base = m.power_of_two_plus_one() ? index : nullptr;
    EmitLea(this, node, index, m.scale(), base, nullptr);
    return;
  }
  X87OperandGenerator g(this);
  Node* left = node->InputAt(0);
  Node* right = node->InputAt(1);
  if (g.CanBeImmediate(right)) {
    Emit(kX87Imul, g.DefineAsRegister(node), g.Use(left),
         g.UseImmediate(right));
  } else {
    if (g.CanBeBetterLeftOperand(right)) {
      std::swap(left, right);
    }
    Emit(kX87Imul, g.DefineSameAsFirst(node), g.UseRegister(left),
         g.Use(right));
  }
}


void InstructionSelector::VisitInt32MulHigh(Node* node) {
  VisitMulHigh(this, node, kX87ImulHigh);
}


void InstructionSelector::VisitUint32MulHigh(Node* node) {
  VisitMulHigh(this, node, kX87UmulHigh);
}


void InstructionSelector::VisitInt32Div(Node* node) {
  VisitDiv(this, node, kX87Idiv);
}


void InstructionSelector::VisitUint32Div(Node* node) {
  VisitDiv(this, node, kX87Udiv);
}


void InstructionSelector::VisitInt32Mod(Node* node) {
  VisitMod(this, node, kX87Idiv);
}


void InstructionSelector::VisitUint32Mod(Node* node) {
  VisitMod(this, node, kX87Udiv);
}


void InstructionSelector::VisitChangeFloat32ToFloat64(Node* node) {
  X87OperandGenerator g(this);
  Emit(kX87Float32ToFloat64, g.DefineAsFixed(node, stX_0),
       g.Use(node->InputAt(0)));
}


void InstructionSelector::VisitRoundInt32ToFloat32(Node* node) {
  X87OperandGenerator g(this);
  Emit(kX87Int32ToFloat32, g.DefineAsFixed(node, stX_0),
       g.Use(node->InputAt(0)));
}


void InstructionSelector::VisitRoundUint32ToFloat32(Node* node) {
  X87OperandGenerator g(this);
  Emit(kX87Uint32ToFloat32, g.DefineAsFixed(node, stX_0),
       g.Use(node->InputAt(0)));
}


void InstructionSelector::VisitChangeInt32ToFloat64(Node* node) {
  X87OperandGenerator g(this);
  Emit(kX87Int32ToFloat64, g.DefineAsFixed(node, stX_0),
       g.Use(node->InputAt(0)));
}


void InstructionSelector::VisitChangeUint32ToFloat64(Node* node) {
  X87OperandGenerator g(this);
  Emit(kX87Uint32ToFloat64, g.DefineAsFixed(node, stX_0),
       g.UseRegister(node->InputAt(0)));
}


void InstructionSelector::VisitTruncateFloat32ToInt32(Node* node) {
  X87OperandGenerator g(this);
  Emit(kX87Float32ToInt32, g.DefineAsRegister(node), g.Use(node->InputAt(0)));
}


void InstructionSelector::VisitTruncateFloat32ToUint32(Node* node) {
  X87OperandGenerator g(this);
  Emit(kX87Float32ToUint32, g.DefineAsRegister(node), g.Use(node->InputAt(0)));
}


void InstructionSelector::VisitChangeFloat64ToInt32(Node* node) {
  X87OperandGenerator g(this);
  Emit(kX87Float64ToInt32, g.DefineAsRegister(node), g.Use(node->InputAt(0)));
}


void InstructionSelector::VisitChangeFloat64ToUint32(Node* node) {
  X87OperandGenerator g(this);
  Emit(kX87Float64ToUint32, g.DefineAsRegister(node), g.Use(node->InputAt(0)));
}

void InstructionSelector::VisitTruncateFloat64ToUint32(Node* node) {
  X87OperandGenerator g(this);
  Emit(kX87Float64ToUint32, g.DefineAsRegister(node), g.Use(node->InputAt(0)));
}

void InstructionSelector::VisitTruncateFloat64ToFloat32(Node* node) {
  X87OperandGenerator g(this);
  Emit(kX87Float64ToFloat32, g.DefineAsFixed(node, stX_0),
       g.Use(node->InputAt(0)));
}

void InstructionSelector::VisitTruncateFloat64ToWord32(Node* node) {
  X87OperandGenerator g(this);
  Emit(kArchTruncateDoubleToI, g.DefineAsRegister(node),
       g.Use(node->InputAt(0)));
}

void InstructionSelector::VisitRoundFloat64ToInt32(Node* node) {
  X87OperandGenerator g(this);
  Emit(kX87Float64ToInt32, g.DefineAsRegister(node), g.Use(node->InputAt(0)));
}


void InstructionSelector::VisitBitcastFloat32ToInt32(Node* node) {
  X87OperandGenerator g(this);
  Emit(kX87PushFloat32, g.NoOutput(), g.Use(node->InputAt(0)));
  Emit(kX87BitcastFI, g.DefineAsRegister(node), 0, nullptr);
}


void InstructionSelector::VisitBitcastInt32ToFloat32(Node* node) {
  X87OperandGenerator g(this);
  Emit(kX87BitcastIF, g.DefineAsFixed(node, stX_0), g.Use(node->InputAt(0)));
}


void InstructionSelector::VisitFloat32Add(Node* node) {
  X87OperandGenerator g(this);
  Emit(kX87PushFloat32, g.NoOutput(), g.Use(node->InputAt(0)));
  Emit(kX87PushFloat32, g.NoOutput(), g.Use(node->InputAt(1)));
  Emit(kX87Float32Add, g.DefineAsFixed(node, stX_0), 0, nullptr);
}


void InstructionSelector::VisitFloat64Add(Node* node) {
  X87OperandGenerator g(this);
  Emit(kX87PushFloat64, g.NoOutput(), g.Use(node->InputAt(0)));
  Emit(kX87PushFloat64, g.NoOutput(), g.Use(node->InputAt(1)));
  Emit(kX87Float64Add, g.DefineAsFixed(node, stX_0), 0, nullptr);
}


void InstructionSelector::VisitFloat32Sub(Node* node) {
  X87OperandGenerator g(this);
  Emit(kX87PushFloat32, g.NoOutput(), g.Use(node->InputAt(0)));
  Emit(kX87PushFloat32, g.NoOutput(), g.Use(node->InputAt(1)));
  Emit(kX87Float32Sub, g.DefineAsFixed(node, stX_0), 0, nullptr);
}

void InstructionSelector::VisitFloat32SubPreserveNan(Node* node) {
  X87OperandGenerator g(this);
  Emit(kX87PushFloat32, g.NoOutput(), g.Use(node->InputAt(0)));
  Emit(kX87PushFloat32, g.NoOutput(), g.Use(node->InputAt(1)));
  Emit(kX87Float32Sub, g.DefineAsFixed(node, stX_0), 0, nullptr);
}

void InstructionSelector::VisitFloat64Sub(Node* node) {
  X87OperandGenerator g(this);
  Emit(kX87PushFloat64, g.NoOutput(), g.Use(node->InputAt(0)));
  Emit(kX87PushFloat64, g.NoOutput(), g.Use(node->InputAt(1)));
  Emit(kX87Float64Sub, g.DefineAsFixed(node, stX_0), 0, nullptr);
}

void InstructionSelector::VisitFloat64SubPreserveNan(Node* node) {
  X87OperandGenerator g(this);
  Emit(kX87PushFloat64, g.NoOutput(), g.Use(node->InputAt(0)));
  Emit(kX87PushFloat64, g.NoOutput(), g.Use(node->InputAt(1)));
  Emit(kX87Float64Sub, g.DefineAsFixed(node, stX_0), 0, nullptr);
}


void InstructionSelector::VisitFloat32Mul(Node* node) {
  X87OperandGenerator g(this);
  Emit(kX87PushFloat32, g.NoOutput(), g.Use(node->InputAt(0)));
  Emit(kX87PushFloat32, g.NoOutput(), g.Use(node->InputAt(1)));
  Emit(kX87Float32Mul, g.DefineAsFixed(node, stX_0), 0, nullptr);
}


void InstructionSelector::VisitFloat64Mul(Node* node) {
  X87OperandGenerator g(this);
  Emit(kX87PushFloat64, g.NoOutput(), g.Use(node->InputAt(0)));
  Emit(kX87PushFloat64, g.NoOutput(), g.Use(node->InputAt(1)));
  Emit(kX87Float64Mul, g.DefineAsFixed(node, stX_0), 0, nullptr);
}


void InstructionSelector::VisitFloat32Div(Node* node) {
  X87OperandGenerator g(this);
  Emit(kX87PushFloat32, g.NoOutput(), g.Use(node->InputAt(0)));
  Emit(kX87PushFloat32, g.NoOutput(), g.Use(node->InputAt(1)));
  Emit(kX87Float32Div, g.DefineAsFixed(node, stX_0), 0, nullptr);
}


void InstructionSelector::VisitFloat64Div(Node* node) {
  X87OperandGenerator g(this);
  Emit(kX87PushFloat64, g.NoOutput(), g.Use(node->InputAt(0)));
  Emit(kX87PushFloat64, g.NoOutput(), g.Use(node->InputAt(1)));
  Emit(kX87Float64Div, g.DefineAsFixed(node, stX_0), 0, nullptr);
}


void InstructionSelector::VisitFloat64Mod(Node* node) {
  X87OperandGenerator g(this);
  InstructionOperand temps[] = {g.TempRegister(eax)};
  Emit(kX87PushFloat64, g.NoOutput(), g.Use(node->InputAt(0)));
  Emit(kX87PushFloat64, g.NoOutput(), g.Use(node->InputAt(1)));
  Emit(kX87Float64Mod, g.DefineAsFixed(node, stX_0), 1, temps)->MarkAsCall();
}


void InstructionSelector::VisitFloat32Max(Node* node) {
  X87OperandGenerator g(this);
  Emit(kX87PushFloat32, g.NoOutput(), g.Use(node->InputAt(0)));
  Emit(kX87PushFloat32, g.NoOutput(), g.Use(node->InputAt(1)));
  Emit(kX87Float32Max, g.DefineAsFixed(node, stX_0), 0, nullptr);
}


void InstructionSelector::VisitFloat64Max(Node* node) {
  X87OperandGenerator g(this);
  Emit(kX87PushFloat64, g.NoOutput(), g.Use(node->InputAt(0)));
  Emit(kX87PushFloat64, g.NoOutput(), g.Use(node->InputAt(1)));
  Emit(kX87Float64Max, g.DefineAsFixed(node, stX_0), 0, nullptr);
}


void InstructionSelector::VisitFloat32Min(Node* node) {
  X87OperandGenerator g(this);
  Emit(kX87PushFloat32, g.NoOutput(), g.Use(node->InputAt(0)));
  Emit(kX87PushFloat32, g.NoOutput(), g.Use(node->InputAt(1)));
  Emit(kX87Float32Min, g.DefineAsFixed(node, stX_0), 0, nullptr);
}


void InstructionSelector::VisitFloat64Min(Node* node) {
  X87OperandGenerator g(this);
  Emit(kX87PushFloat64, g.NoOutput(), g.Use(node->InputAt(0)));
  Emit(kX87PushFloat64, g.NoOutput(), g.Use(node->InputAt(1)));
  Emit(kX87Float64Min, g.DefineAsFixed(node, stX_0), 0, nullptr);
}


void InstructionSelector::VisitFloat32Abs(Node* node) {
  X87OperandGenerator g(this);
  Emit(kX87PushFloat32, g.NoOutput(), g.Use(node->InputAt(0)));
  Emit(kX87Float32Abs, g.DefineAsFixed(node, stX_0), 0, nullptr);
}


void InstructionSelector::VisitFloat64Abs(Node* node) {
  X87OperandGenerator g(this);
  Emit(kX87PushFloat64, g.NoOutput(), g.Use(node->InputAt(0)));
  Emit(kX87Float64Abs, g.DefineAsFixed(node, stX_0), 0, nullptr);
}


void InstructionSelector::VisitFloat32Sqrt(Node* node) {
  X87OperandGenerator g(this);
  Emit(kX87PushFloat32, g.NoOutput(), g.Use(node->InputAt(0)));
  Emit(kX87Float32Sqrt, g.DefineAsFixed(node, stX_0), 0, nullptr);
}


void InstructionSelector::VisitFloat64Sqrt(Node* node) {
  X87OperandGenerator g(this);
  Emit(kX87PushFloat64, g.NoOutput(), g.Use(node->InputAt(0)));
  Emit(kX87Float64Sqrt, g.DefineAsFixed(node, stX_0), 0, nullptr);
}


void InstructionSelector::VisitFloat32RoundDown(Node* node) {
  X87OperandGenerator g(this);
  Emit(kX87Float32Round | MiscField::encode(kRoundDown),
       g.UseFixed(node, stX_0), g.Use(node->InputAt(0)));
}


void InstructionSelector::VisitFloat64RoundDown(Node* node) {
  X87OperandGenerator g(this);
  Emit(kX87Float64Round | MiscField::encode(kRoundDown),
       g.UseFixed(node, stX_0), g.Use(node->InputAt(0)));
}


void InstructionSelector::VisitFloat32RoundUp(Node* node) {
  X87OperandGenerator g(this);
  Emit(kX87Float32Round | MiscField::encode(kRoundUp), g.UseFixed(node, stX_0),
       g.Use(node->InputAt(0)));
}


void InstructionSelector::VisitFloat64RoundUp(Node* node) {
  X87OperandGenerator g(this);
  Emit(kX87Float64Round | MiscField::encode(kRoundUp), g.UseFixed(node, stX_0),
       g.Use(node->InputAt(0)));
}


void InstructionSelector::VisitFloat32RoundTruncate(Node* node) {
  X87OperandGenerator g(this);
  Emit(kX87Float32Round | MiscField::encode(kRoundToZero),
       g.UseFixed(node, stX_0), g.Use(node->InputAt(0)));
}


void InstructionSelector::VisitFloat64RoundTruncate(Node* node) {
  X87OperandGenerator g(this);
  Emit(kX87Float64Round | MiscField::encode(kRoundToZero),
       g.UseFixed(node, stX_0), g.Use(node->InputAt(0)));
}


void InstructionSelector::VisitFloat64RoundTiesAway(Node* node) {
  UNREACHABLE();
}


void InstructionSelector::VisitFloat32RoundTiesEven(Node* node) {
  X87OperandGenerator g(this);
  Emit(kX87Float32Round | MiscField::encode(kRoundToNearest),
       g.UseFixed(node, stX_0), g.Use(node->InputAt(0)));
}


void InstructionSelector::VisitFloat64RoundTiesEven(Node* node) {
  X87OperandGenerator g(this);
  Emit(kX87Float64Round | MiscField::encode(kRoundToNearest),
       g.UseFixed(node, stX_0), g.Use(node->InputAt(0)));
}


void InstructionSelector::EmitPrepareArguments(
    ZoneVector<PushParameter>* arguments, const CallDescriptor* descriptor,
    Node* node) {
  X87OperandGenerator g(this);

  // Prepare for C function call.
  if (descriptor->IsCFunctionCall()) {
    InstructionOperand temps[] = {g.TempRegister()};
    size_t const temp_count = arraysize(temps);
    Emit(kArchPrepareCallCFunction |
             MiscField::encode(static_cast<int>(descriptor->CParameterCount())),
         0, nullptr, 0, nullptr, temp_count, temps);

    // Poke any stack arguments.
    for (size_t n = 0; n < arguments->size(); ++n) {
      PushParameter input = (*arguments)[n];
      if (input.node()) {
        int const slot = static_cast<int>(n);
        InstructionOperand value = g.CanBeImmediate(input.node())
                                       ? g.UseImmediate(input.node())
                                       : g.UseRegister(input.node());
        Emit(kX87Poke | MiscField::encode(slot), g.NoOutput(), value);
      }
    }
  } else {
    // Push any stack arguments.
    for (PushParameter input : base::Reversed(*arguments)) {
      // TODO(titzer): handle pushing double parameters.
      if (input.node() == nullptr) continue;
      InstructionOperand value =
          g.CanBeImmediate(input.node())
              ? g.UseImmediate(input.node())
              : IsSupported(ATOM) ||
                        sequence()->IsFloat(GetVirtualRegister(input.node()))
                    ? g.UseRegister(input.node())
                    : g.Use(input.node());
      Emit(kX87Push, g.NoOutput(), value);
    }
  }
}


bool InstructionSelector::IsTailCallAddressImmediate() { return true; }

int InstructionSelector::GetTempsCountForTailCallFromJSFunction() { return 0; }

namespace {

void VisitCompareWithMemoryOperand(InstructionSelector* selector,
                                   InstructionCode opcode, Node* left,
                                   InstructionOperand right,
                                   FlagsContinuation* cont) {
  DCHECK(left->opcode() == IrOpcode::kLoad);
  X87OperandGenerator g(selector);
  size_t input_count = 0;
  InstructionOperand inputs[6];
  AddressingMode addressing_mode =
      g.GetEffectiveAddressMemoryOperand(left, inputs, &input_count);
  opcode |= AddressingModeField::encode(addressing_mode);
  opcode = cont->Encode(opcode);
  inputs[input_count++] = right;

  if (cont->IsBranch()) {
    inputs[input_count++] = g.Label(cont->true_block());
    inputs[input_count++] = g.Label(cont->false_block());
    selector->Emit(opcode, 0, nullptr, input_count, inputs);
  } else if (cont->IsDeoptimize()) {
    selector->EmitDeoptimize(opcode, 0, nullptr, input_count, inputs,
                             cont->frame_state());
  } else {
    DCHECK(cont->IsSet());
    InstructionOperand output = g.DefineAsRegister(cont->result());
    selector->Emit(opcode, 1, &output, input_count, inputs);
  }
}

// Shared routine for multiple compare operations.
void VisitCompare(InstructionSelector* selector, InstructionCode opcode,
                  InstructionOperand left, InstructionOperand right,
                  FlagsContinuation* cont) {
  X87OperandGenerator g(selector);
  opcode = cont->Encode(opcode);
  if (cont->IsBranch()) {
    selector->Emit(opcode, g.NoOutput(), left, right,
                   g.Label(cont->true_block()), g.Label(cont->false_block()));
  } else if (cont->IsDeoptimize()) {
    selector->EmitDeoptimize(opcode, g.NoOutput(), left, right,
                             cont->frame_state());
  } else {
    DCHECK(cont->IsSet());
    selector->Emit(opcode, g.DefineAsByteRegister(cont->result()), left, right);
  }
}


// Shared routine for multiple compare operations.
void VisitCompare(InstructionSelector* selector, InstructionCode opcode,
                  Node* left, Node* right, FlagsContinuation* cont,
                  bool commutative) {
  X87OperandGenerator g(selector);
  if (commutative && g.CanBeBetterLeftOperand(right)) {
    std::swap(left, right);
  }
  VisitCompare(selector, opcode, g.UseRegister(left), g.Use(right), cont);
}

bool InferMachineRepresentation(Node* node,
                                MachineRepresentation* representation) {
  if (node->opcode() == IrOpcode::kLoad) {
    *representation = LoadRepresentationOf(node->op()).representation();
    return true;
  }
  if (node->opcode() != IrOpcode::kInt32Constant) {
    return false;
  }
  int32_t value = OpParameter<int32_t>(node->op());
  if (is_int8(value)) {
    *representation = MachineRepresentation::kWord8;
  } else if (is_int16(value)) {
    *representation = MachineRepresentation::kWord16;
  } else {
    *representation = MachineRepresentation::kWord32;
  }
  return true;
}

// Tries to match the size of the given opcode to that of the operands, if
// possible.
InstructionCode TryNarrowOpcodeSize(InstructionCode opcode, Node* left,
                                    Node* right) {
  if (opcode != kX87Cmp && opcode != kX87Test) {
    return opcode;
  }
  // We only do this if at least one of the two operands is a load.
  // TODO(epertoso): we can probably get some size information out of phi nodes.
  if (left->opcode() != IrOpcode::kLoad && right->opcode() != IrOpcode::kLoad) {
    return opcode;
  }
  MachineRepresentation left_representation, right_representation;
  if (!InferMachineRepresentation(left, &left_representation) ||
      !InferMachineRepresentation(right, &right_representation)) {
    return opcode;
  }
  // If the representations don't match, both operands will be
  // zero/sign-extended to 32bit.
  if (left_representation != right_representation) {
    return opcode;
  }
  switch (left_representation) {
    case MachineRepresentation::kBit:
    case MachineRepresentation::kWord8:
      return opcode == kX87Cmp ? kX87Cmp8 : kX87Test8;
    case MachineRepresentation::kWord16:
      return opcode == kX87Cmp ? kX87Cmp16 : kX87Test16;
    default:
      return opcode;
  }
}

// Shared routine for multiple float32 compare operations (inputs commuted).
void VisitFloat32Compare(InstructionSelector* selector, Node* node,
                         FlagsContinuation* cont) {
  X87OperandGenerator g(selector);
  selector->Emit(kX87PushFloat32, g.NoOutput(), g.Use(node->InputAt(0)));
  selector->Emit(kX87PushFloat32, g.NoOutput(), g.Use(node->InputAt(1)));
  if (cont->IsBranch()) {
    selector->Emit(cont->Encode(kX87Float32Cmp), g.NoOutput(),
                   g.Label(cont->true_block()), g.Label(cont->false_block()));
  } else if (cont->IsDeoptimize()) {
    selector->EmitDeoptimize(cont->Encode(kX87Float32Cmp), g.NoOutput(),
                             g.Use(node->InputAt(0)), g.Use(node->InputAt(1)),
                             cont->frame_state());
  } else {
    DCHECK(cont->IsSet());
    selector->Emit(cont->Encode(kX87Float32Cmp),
                   g.DefineAsByteRegister(cont->result()));
  }
}


// Shared routine for multiple float64 compare operations (inputs commuted).
void VisitFloat64Compare(InstructionSelector* selector, Node* node,
                         FlagsContinuation* cont) {
  X87OperandGenerator g(selector);
  selector->Emit(kX87PushFloat64, g.NoOutput(), g.Use(node->InputAt(0)));
  selector->Emit(kX87PushFloat64, g.NoOutput(), g.Use(node->InputAt(1)));
  if (cont->IsBranch()) {
    selector->Emit(cont->Encode(kX87Float64Cmp), g.NoOutput(),
                   g.Label(cont->true_block()), g.Label(cont->false_block()));
  } else if (cont->IsDeoptimize()) {
    selector->EmitDeoptimize(cont->Encode(kX87Float64Cmp), g.NoOutput(),
                             g.Use(node->InputAt(0)), g.Use(node->InputAt(1)),
                             cont->frame_state());
  } else {
    DCHECK(cont->IsSet());
    selector->Emit(cont->Encode(kX87Float64Cmp),
                   g.DefineAsByteRegister(cont->result()));
  }
}

// Shared routine for multiple word compare operations.
void VisitWordCompare(InstructionSelector* selector, Node* node,
                      InstructionCode opcode, FlagsContinuation* cont) {
  X87OperandGenerator g(selector);
  Node* left = node->InputAt(0);
  Node* right = node->InputAt(1);

  InstructionCode narrowed_opcode = TryNarrowOpcodeSize(opcode, left, right);

  // If one of the two inputs is an immediate, make sure it's on the right, or
  // if one of the two inputs is a memory operand, make sure it's on the left.
  if ((!g.CanBeImmediate(right) && g.CanBeImmediate(left)) ||
      (g.CanBeMemoryOperand(narrowed_opcode, node, right) &&
       !g.CanBeMemoryOperand(narrowed_opcode, node, left))) {
    if (!node->op()->HasProperty(Operator::kCommutative)) cont->Commute();
    std::swap(left, right);
  }

  // Match immediates on right side of comparison.
  if (g.CanBeImmediate(right)) {
    if (g.CanBeMemoryOperand(narrowed_opcode, node, left)) {
      // If we're truncating the immediate (32 bits to 16 or 8), comparison
      // semantics should take the signedness/unsignedness of the op into
      // account.
      if (narrowed_opcode != opcode &&
          LoadRepresentationOf(left->op()).IsUnsigned()) {
        switch (cont->condition()) {
          case FlagsCondition::kSignedLessThan:
            cont->OverwriteAndNegateIfEqual(FlagsCondition::kUnsignedLessThan);
            break;
          case FlagsCondition::kSignedGreaterThan:
            cont->OverwriteAndNegateIfEqual(
                FlagsCondition::kUnsignedGreaterThan);
            break;
          case FlagsCondition::kSignedLessThanOrEqual:
            cont->OverwriteAndNegateIfEqual(
                FlagsCondition::kUnsignedLessThanOrEqual);
            break;
          case FlagsCondition::kSignedGreaterThanOrEqual:
            cont->OverwriteAndNegateIfEqual(
                FlagsCondition::kUnsignedGreaterThanOrEqual);
            break;
          default:
            break;
        }
      }
      return VisitCompareWithMemoryOperand(selector, narrowed_opcode, left,
                                           g.UseImmediate(right), cont);
    }
    return VisitCompare(selector, opcode, g.Use(left), g.UseImmediate(right),
                        cont);
  }

  // Match memory operands on left side of comparison.
  if (g.CanBeMemoryOperand(narrowed_opcode, node, left)) {
    bool needs_byte_register =
        narrowed_opcode == kX87Test8 || narrowed_opcode == kX87Cmp8;
    return VisitCompareWithMemoryOperand(
        selector, narrowed_opcode, left,
        needs_byte_register ? g.UseByteRegister(right) : g.UseRegister(right),
        cont);
  }

  if (g.CanBeBetterLeftOperand(right)) {
    if (!node->op()->HasProperty(Operator::kCommutative)) cont->Commute();
    std::swap(left, right);
  }

  return VisitCompare(selector, opcode, left, right, cont,
                      node->op()->HasProperty(Operator::kCommutative));
}

void VisitWordCompare(InstructionSelector* selector, Node* node,
                      FlagsContinuation* cont) {
  X87OperandGenerator g(selector);
  Int32BinopMatcher m(node);
  if (m.left().IsLoad() && m.right().IsLoadStackPointer()) {
    LoadMatcher<ExternalReferenceMatcher> mleft(m.left().node());
    ExternalReference js_stack_limit =
        ExternalReference::address_of_stack_limit(selector->isolate());
    if (mleft.object().Is(js_stack_limit) && mleft.index().Is(0)) {
      // Compare(Load(js_stack_limit), LoadStackPointer)
      if (!node->op()->HasProperty(Operator::kCommutative)) cont->Commute();
      InstructionCode opcode = cont->Encode(kX87StackCheck);
      if (cont->IsBranch()) {
        selector->Emit(opcode, g.NoOutput(), g.Label(cont->true_block()),
                       g.Label(cont->false_block()));
      } else if (cont->IsDeoptimize()) {
        selector->EmitDeoptimize(opcode, 0, nullptr, 0, nullptr,
                                 cont->frame_state());
      } else {
        DCHECK(cont->IsSet());
        selector->Emit(opcode, g.DefineAsRegister(cont->result()));
      }
      return;
    }
  }
  VisitWordCompare(selector, node, kX87Cmp, cont);
}


// Shared routine for word comparison with zero.
void VisitWordCompareZero(InstructionSelector* selector, Node* user,
                          Node* value, FlagsContinuation* cont) {
  // Try to combine the branch with a comparison.
  while (selector->CanCover(user, value)) {
    switch (value->opcode()) {
      case IrOpcode::kWord32Equal: {
        // Try to combine with comparisons against 0 by simply inverting the
        // continuation.
        Int32BinopMatcher m(value);
        if (m.right().Is(0)) {
          user = value;
          value = m.left().node();
          cont->Negate();
          continue;
        }
        cont->OverwriteAndNegateIfEqual(kEqual);
        return VisitWordCompare(selector, value, cont);
      }
      case IrOpcode::kInt32LessThan:
        cont->OverwriteAndNegateIfEqual(kSignedLessThan);
        return VisitWordCompare(selector, value, cont);
      case IrOpcode::kInt32LessThanOrEqual:
        cont->OverwriteAndNegateIfEqual(kSignedLessThanOrEqual);
        return VisitWordCompare(selector, value, cont);
      case IrOpcode::kUint32LessThan:
        cont->OverwriteAndNegateIfEqual(kUnsignedLessThan);
        return VisitWordCompare(selector, value, cont);
      case IrOpcode::kUint32LessThanOrEqual:
        cont->OverwriteAndNegateIfEqual(kUnsignedLessThanOrEqual);
        return VisitWordCompare(selector, value, cont);
      case IrOpcode::kFloat32Equal:
        cont->OverwriteAndNegateIfEqual(kUnorderedEqual);
        return VisitFloat32Compare(selector, value, cont);
      case IrOpcode::kFloat32LessThan:
        cont->OverwriteAndNegateIfEqual(kUnsignedGreaterThan);
        return VisitFloat32Compare(selector, value, cont);
      case IrOpcode::kFloat32LessThanOrEqual:
        cont->OverwriteAndNegateIfEqual(kUnsignedGreaterThanOrEqual);
        return VisitFloat32Compare(selector, value, cont);
      case IrOpcode::kFloat64Equal:
        cont->OverwriteAndNegateIfEqual(kUnorderedEqual);
        return VisitFloat64Compare(selector, value, cont);
      case IrOpcode::kFloat64LessThan:
        cont->OverwriteAndNegateIfEqual(kUnsignedGreaterThan);
        return VisitFloat64Compare(selector, value, cont);
      case IrOpcode::kFloat64LessThanOrEqual:
        cont->OverwriteAndNegateIfEqual(kUnsignedGreaterThanOrEqual);
        return VisitFloat64Compare(selector, value, cont);
      case IrOpcode::kProjection:
        // Check if this is the overflow output projection of an
        // <Operation>WithOverflow node.
        if (ProjectionIndexOf(value->op()) == 1u) {
          // We cannot combine the <Operation>WithOverflow with this branch
          // unless the 0th projection (the use of the actual value of the
          // <Operation> is either nullptr, which means there's no use of the
          // actual value, or was already defined, which means it is scheduled
          // *AFTER* this branch).
          Node* const node = value->InputAt(0);
          Node* const result = NodeProperties::FindProjection(node, 0);
          if (result == nullptr || selector->IsDefined(result)) {
            switch (node->opcode()) {
              case IrOpcode::kInt32AddWithOverflow:
                cont->OverwriteAndNegateIfEqual(kOverflow);
                return VisitBinop(selector, node, kX87Add, cont);
              case IrOpcode::kInt32SubWithOverflow:
                cont->OverwriteAndNegateIfEqual(kOverflow);
                return VisitBinop(selector, node, kX87Sub, cont);
              default:
                break;
            }
          }
        }
        break;
      case IrOpcode::kInt32Sub:
        return VisitWordCompare(selector, value, cont);
      case IrOpcode::kWord32And:
        return VisitWordCompare(selector, value, kX87Test, cont);
      default:
        break;
    }
    break;
  }

  // Continuation could not be combined with a compare, emit compare against 0.
  X87OperandGenerator g(selector);
  VisitCompare(selector, kX87Cmp, g.Use(value), g.TempImmediate(0), cont);
}

}  // namespace


void InstructionSelector::VisitBranch(Node* branch, BasicBlock* tbranch,
                                      BasicBlock* fbranch) {
  FlagsContinuation cont(kNotEqual, tbranch, fbranch);
  VisitWordCompareZero(this, branch, branch->InputAt(0), &cont);
}

void InstructionSelector::VisitDeoptimizeIf(Node* node) {
  FlagsContinuation cont =
      FlagsContinuation::ForDeoptimize(kNotEqual, node->InputAt(1));
  VisitWordCompareZero(this, node, node->InputAt(0), &cont);
}

void InstructionSelector::VisitDeoptimizeUnless(Node* node) {
  FlagsContinuation cont =
      FlagsContinuation::ForDeoptimize(kEqual, node->InputAt(1));
  VisitWordCompareZero(this, node, node->InputAt(0), &cont);
}

void InstructionSelector::VisitSwitch(Node* node, const SwitchInfo& sw) {
  X87OperandGenerator g(this);
  InstructionOperand value_operand = g.UseRegister(node->InputAt(0));

  // Emit either ArchTableSwitch or ArchLookupSwitch.
  size_t table_space_cost = 4 + sw.value_range;
  size_t table_time_cost = 3;
  size_t lookup_space_cost = 3 + 2 * sw.case_count;
  size_t lookup_time_cost = sw.case_count;
  if (sw.case_count > 4 &&
      table_space_cost + 3 * table_time_cost <=
          lookup_space_cost + 3 * lookup_time_cost &&
      sw.min_value > std::numeric_limits<int32_t>::min()) {
    InstructionOperand index_operand = value_operand;
    if (sw.min_value) {
      index_operand = g.TempRegister();
      Emit(kX87Lea | AddressingModeField::encode(kMode_MRI), index_operand,
           value_operand, g.TempImmediate(-sw.min_value));
    }
    // Generate a table lookup.
    return EmitTableSwitch(sw, index_operand);
  }

  // Generate a sequence of conditional jumps.
  return EmitLookupSwitch(sw, value_operand);
}


void InstructionSelector::VisitWord32Equal(Node* const node) {
  FlagsContinuation cont = FlagsContinuation::ForSet(kEqual, node);
  Int32BinopMatcher m(node);
  if (m.right().Is(0)) {
    return VisitWordCompareZero(this, m.node(), m.left().node(), &cont);
  }
  VisitWordCompare(this, node, &cont);
}


void InstructionSelector::VisitInt32LessThan(Node* node) {
  FlagsContinuation cont = FlagsContinuation::ForSet(kSignedLessThan, node);
  VisitWordCompare(this, node, &cont);
}


void InstructionSelector::VisitInt32LessThanOrEqual(Node* node) {
  FlagsContinuation cont =
      FlagsContinuation::ForSet(kSignedLessThanOrEqual, node);
  VisitWordCompare(this, node, &cont);
}


void InstructionSelector::VisitUint32LessThan(Node* node) {
  FlagsContinuation cont = FlagsContinuation::ForSet(kUnsignedLessThan, node);
  VisitWordCompare(this, node, &cont);
}


void InstructionSelector::VisitUint32LessThanOrEqual(Node* node) {
  FlagsContinuation cont =
      FlagsContinuation::ForSet(kUnsignedLessThanOrEqual, node);
  VisitWordCompare(this, node, &cont);
}


void InstructionSelector::VisitInt32AddWithOverflow(Node* node) {
  if (Node* ovf = NodeProperties::FindProjection(node, 1)) {
    FlagsContinuation cont = FlagsContinuation::ForSet(kOverflow, ovf);
    return VisitBinop(this, node, kX87Add, &cont);
  }
  FlagsContinuation cont;
  VisitBinop(this, node, kX87Add, &cont);
}


void InstructionSelector::VisitInt32SubWithOverflow(Node* node) {
  if (Node* ovf = NodeProperties::FindProjection(node, 1)) {
    FlagsContinuation cont = FlagsContinuation::ForSet(kOverflow, ovf);
    return VisitBinop(this, node, kX87Sub, &cont);
  }
  FlagsContinuation cont;
  VisitBinop(this, node, kX87Sub, &cont);
}


void InstructionSelector::VisitFloat32Equal(Node* node) {
  FlagsContinuation cont = FlagsContinuation::ForSet(kUnorderedEqual, node);
  VisitFloat32Compare(this, node, &cont);
}


void InstructionSelector::VisitFloat32LessThan(Node* node) {
  FlagsContinuation cont =
      FlagsContinuation::ForSet(kUnsignedGreaterThan, node);
  VisitFloat32Compare(this, node, &cont);
}


void InstructionSelector::VisitFloat32LessThanOrEqual(Node* node) {
  FlagsContinuation cont =
      FlagsContinuation::ForSet(kUnsignedGreaterThanOrEqual, node);
  VisitFloat32Compare(this, node, &cont);
}


void InstructionSelector::VisitFloat64Equal(Node* node) {
  FlagsContinuation cont = FlagsContinuation::ForSet(kUnorderedEqual, node);
  VisitFloat64Compare(this, node, &cont);
}


void InstructionSelector::VisitFloat64LessThan(Node* node) {
  FlagsContinuation cont =
      FlagsContinuation::ForSet(kUnsignedGreaterThan, node);
  VisitFloat64Compare(this, node, &cont);
}


void InstructionSelector::VisitFloat64LessThanOrEqual(Node* node) {
  FlagsContinuation cont =
      FlagsContinuation::ForSet(kUnsignedGreaterThanOrEqual, node);
  VisitFloat64Compare(this, node, &cont);
}


void InstructionSelector::VisitFloat64ExtractLowWord32(Node* node) {
  X87OperandGenerator g(this);
  Emit(kX87Float64ExtractLowWord32, g.DefineAsRegister(node),
       g.Use(node->InputAt(0)));
}


void InstructionSelector::VisitFloat64ExtractHighWord32(Node* node) {
  X87OperandGenerator g(this);
  Emit(kX87Float64ExtractHighWord32, g.DefineAsRegister(node),
       g.Use(node->InputAt(0)));
}


void InstructionSelector::VisitFloat64InsertLowWord32(Node* node) {
  X87OperandGenerator g(this);
  Node* left = node->InputAt(0);
  Node* right = node->InputAt(1);
  Emit(kX87Float64InsertLowWord32, g.UseFixed(node, stX_0), g.UseRegister(left),
       g.UseRegister(right));
}


void InstructionSelector::VisitFloat64InsertHighWord32(Node* node) {
  X87OperandGenerator g(this);
  Node* left = node->InputAt(0);
  Node* right = node->InputAt(1);
  Emit(kX87Float64InsertHighWord32, g.UseFixed(node, stX_0),
       g.UseRegister(left), g.UseRegister(right));
}

void InstructionSelector::VisitAtomicLoad(Node* node) {
  LoadRepresentation load_rep = LoadRepresentationOf(node->op());
  DCHECK(load_rep.representation() == MachineRepresentation::kWord8 ||
         load_rep.representation() == MachineRepresentation::kWord16 ||
         load_rep.representation() == MachineRepresentation::kWord32);
  USE(load_rep);
  VisitLoad(node);
}

void InstructionSelector::VisitAtomicStore(Node* node) {
  X87OperandGenerator g(this);
  Node* base = node->InputAt(0);
  Node* index = node->InputAt(1);
  Node* value = node->InputAt(2);

  MachineRepresentation rep = AtomicStoreRepresentationOf(node->op());
  ArchOpcode opcode = kArchNop;
  switch (rep) {
    case MachineRepresentation::kWord8:
      opcode = kX87Xchgb;
      break;
    case MachineRepresentation::kWord16:
      opcode = kX87Xchgw;
      break;
    case MachineRepresentation::kWord32:
      opcode = kX87Xchgl;
      break;
    default:
      UNREACHABLE();
      break;
  }
  AddressingMode addressing_mode;
  InstructionOperand inputs[4];
  size_t input_count = 0;
  inputs[input_count++] = g.UseUniqueRegister(base);
  if (g.CanBeImmediate(index)) {
    inputs[input_count++] = g.UseImmediate(index);
    addressing_mode = kMode_MRI;
  } else {
    inputs[input_count++] = g.UseUniqueRegister(index);
    addressing_mode = kMode_MR1;
  }
  inputs[input_count++] = g.UseUniqueRegister(value);
  InstructionCode code = opcode | AddressingModeField::encode(addressing_mode);
  Emit(code, 0, nullptr, input_count, inputs);
}

// static
MachineOperatorBuilder::Flags
InstructionSelector::SupportedMachineOperatorFlags() {
  MachineOperatorBuilder::Flags flags =
      MachineOperatorBuilder::kFloat32Max |
      MachineOperatorBuilder::kFloat32Min |
      MachineOperatorBuilder::kFloat64Max |
      MachineOperatorBuilder::kFloat64Min |
      MachineOperatorBuilder::kWord32ShiftIsSafe;
  if (CpuFeatures::IsSupported(POPCNT)) {
    flags |= MachineOperatorBuilder::kWord32Popcnt;
  }

  flags |= MachineOperatorBuilder::kFloat32RoundDown |
           MachineOperatorBuilder::kFloat64RoundDown |
           MachineOperatorBuilder::kFloat32RoundUp |
           MachineOperatorBuilder::kFloat64RoundUp |
           MachineOperatorBuilder::kFloat32RoundTruncate |
           MachineOperatorBuilder::kFloat64RoundTruncate |
           MachineOperatorBuilder::kFloat32RoundTiesEven |
           MachineOperatorBuilder::kFloat64RoundTiesEven;
  return flags;
}

}  // namespace compiler
}  // namespace internal
}  // namespace v8
