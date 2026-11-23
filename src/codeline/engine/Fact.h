// A vital boolean from outside this plant that a route reads, sampled once
// per scan. Harvested from FieldUnit src/Fact.h (ADR 0007/0008).
//
// Today a route reads one: whether the next signal ahead is proven
// displaying proceed (the DR term). The CP Corporal slice has no
// neighbouring plant, so scenarios supply a ConstantFact.
#pragma once

namespace codeline::engine {

struct Fact {
  virtual ~Fact() = default;
  virtual bool asserted() const = 0;
};

class ConstantFact final : public Fact {
public:
  explicit ConstantFact(bool value) : value_(value) {}
  void set(bool value) { value_ = value; }
  bool asserted() const override { return value_; }

private:
  bool value_;
};

}  // namespace codeline::engine
