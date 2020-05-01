#include <gtest/gtest.h>

#include "cinn/cinn.h"

namespace cinn {

Expr batch(256);
Expr in_channel(256);
Expr out_channel(512);
Expr in_size(14);
Expr kernel(3);
Expr pad(1);
Expr stride(1);

TEST(test03_conv, basic) {
  Placeholder<float> A("A", {in_size, in_size, in_channel, batch});
  Placeholder<float> W("W", {kernel, kernel, in_channel, out_channel});
  Expr out_size = (in_size - kernel + 2 * pad) / stride + 1;

  auto Apad = Compute(
      {in_size + 2 * pad, in_size + 2 * pad, in_channel, batch},
      [&](Expr yy, Expr xx, Expr cc, Expr nn) {
        auto cond = logic_and({yy >= pad, yy - pad < in_size, xx >= pad, xx - pad < in_size});
        return ir::Select::Make(cond, A(yy - pad, xx - pad, cc, nn), Expr(0.f));
      },
      "Apad");
  Buffer Apad_buf(Apad->type());
  Apad->Bind(Apad_buf);

  Var rc(Expr(0), Expr(in_channel), "rc");
  Var ry(Expr(0), Expr(kernel), "ry");
  Var rx(Expr(0), Expr(kernel), "rx");

  auto B = Compute({out_size, out_size, out_channel, batch},
                   [&](Expr yy, Expr xx, Expr ff, Expr nn) {
                     return Sum(Apad(yy * stride + ry, xx * stride + rx, rc, nn) * W(ry, rx, rc, ff));
                   },
                   "B",
                   {ry, rx, rc});
  Buffer B_buf(Apad->type());
  B->Bind(B_buf);

  Target target(Target::OS::Linux, Target::Arch::X86, Target::Bit::k64);

  Module module("conv", target);
  auto func = Lower("conv", {A, W, Apad, B});

  module.Append(func);

  CodeGenCX86 compiler(target, CodeGenCX86::Feature::AVX256);
  Outputs outputs;
  outputs = outputs.c_header("./test03_convolution.h").c_source("./test03_convolution.cc");
  compiler.Compile(module, outputs);
}

}  // namespace cinn