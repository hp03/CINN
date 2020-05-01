#include "hlir/instruction/optimizer.h"

#include <gtest/gtest.h>

#include "hlir/instruction/lower.h"

namespace hlir {
namespace instruction {

TEST(Optimizer, display) {
  Context context;

  Computation::Builder builder(&context, "add_computation");
  ParameterConfig parameter_config = {Float(32)};

  auto x = builder.AddInstruction(Instruction::CreateParameter(0, Shape({20, 40}), "X", parameter_config));
  auto y = builder.AddInstruction(Instruction::CreateParameter(0, Shape({20, 40}), "y", parameter_config));

  auto add = builder.AddInstruction(Instruction::CreateBinary(x->shape(), InstrCode::Add, x, y));

  Module module("module0");
  module.AddComputation(builder.Build());

  Optimizer optimizer;
  optimizer.Run(&module);
}

}  // namespace instruction
}  // namespace hlir