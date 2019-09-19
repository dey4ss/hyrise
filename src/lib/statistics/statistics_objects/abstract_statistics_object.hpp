#pragma once

#include <memory>
#include <optional>
#include <utility>

#include "all_type_variant.hpp"
#include "types.hpp"

namespace opossum {

/**
 * Base class for types that hold statistical information about a column/segment of data.
 */
class AbstractStatisticsObject : private Noncopyable {
 public:
  explicit AbstractStatisticsObject(const DataType data_type);
  virtual ~AbstractStatisticsObject() = default;

  /**
   * @return A statistics object that represents the data after the predicate has been applied.
   */
  virtual std::shared_ptr<AbstractStatisticsObject> sliced(
      const PredicateCondition predicate_condition, const AllTypeVariant& variant_value,
      const std::optional<AllTypeVariant>& variant_value2 = std::nullopt) const = 0;

  /**
   * @return A statistics object that represents the data after `num_values_pruned` rows that satisfy the given condition
   * have been removed
   */
  virtual std::shared_ptr<AbstractStatisticsObject> pruned(
      const PredicateCondition predicate_condition, const size_t num_values_pruned, const AllTypeVariant& variant_value,
      const std::optional<AllTypeVariant>& variant_value2 = std::nullopt) const;

  /**
   * @return a statistics object that represents the data after a filter with the given selectivity has been applied.
   */
  virtual std::shared_ptr<AbstractStatisticsObject> scaled(const Selectivity selectivity) const = 0;

  /**
   * DataType of the data that this statistics object represents
   */
  const DataType data_type;
};

}  // namespace opossum
