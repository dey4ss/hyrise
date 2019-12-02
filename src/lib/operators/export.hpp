#pragma once

#include <memory>
#include <string>
#include <vector>

#include "abstract_read_only_operator.hpp"
#include "storage/dictionary_segment.hpp"
#include "storage/reference_segment.hpp"
#include "storage/run_length_segment.hpp"
#include "storage/value_segment.hpp"
#include "utils/assert.hpp"

namespace opossum {

class BaseCompressedVector;
enum class CompressedVectorType : uint8_t;

/**
 * Note: ExportBinary does not support null values at the moment
 */
class Export : public AbstractReadOnlyOperator {
 public:
  explicit Export(const std::shared_ptr<const AbstractOperator>& in, const std::string& filename);

  static void write(const Table& table, const std::string& filename);

  /**
   * Executes the export operator
   * @return The table that was also the input
   */
  std::shared_ptr<const Table> _on_execute() final;

  /**
   * Name of the operator is Export
   */
  const std::string& name() const final;

 protected:
  std::shared_ptr<AbstractOperator> _on_deep_copy(
      const std::shared_ptr<AbstractOperator>& copied_input_left,
      const std::shared_ptr<AbstractOperator>& copied_input_right) const override;
  void _on_set_parameters(const std::unordered_map<ParameterID, AllTypeVariant>& parameters) override;

 private:
  // Path of the binary file
  const std::string _filename;
};
}  // namespace opossum
