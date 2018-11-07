#include "jit_read_value.hpp"

#include "constant_mappings.hpp"
#include "jit_segment_reader.hpp"

namespace opossum {

std::string JitReadValue::description() const {
  std::stringstream desc;
  desc << "[ReadValue] x" << _input_column.tuple_value.tuple_index() << " = Col#" << _input_column.column_id << ", ";
  return desc.str();
}

#if JIT_OLD_LAZY_LOAD
void JitReadValue::consume(JitRuntimeContext& context) const {
  _input_segment_wrapper->increment(context);
  if (_incr) _incr->consume(context);
}
#endif

void JitReadValue::_consume(JitRuntimeContext& context) const {
  _input_segment_wrapper->read_value(context);
  _emit(context);
}

}  // namespace opossum