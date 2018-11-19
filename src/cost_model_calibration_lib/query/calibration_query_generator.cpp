#include "calibration_query_generator.hpp"

#include <algorithm>
#include <random>

#include "../configuration/calibration_table_specification.hpp"
#include "calibration_query_generator_aggregates.hpp"
#include "calibration_query_generator_join.hpp"
#include "calibration_query_generator_predicate.hpp"
#include "calibration_query_generator_projection.hpp"
#include "expression/expression_functional.hpp"
#include "logical_query_plan/validate_node.hpp"

namespace opossum {

CalibrationQueryGenerator::CalibrationQueryGenerator(
        const std::vector<std::string>& table_names,
        const std::vector<CalibrationColumnSpecification>& column_specifications,
        const CalibrationConfiguration& configuration)
        : _column_specifications(column_specifications), _configuration(configuration),_table_names(table_names){};

const std::vector<CalibrationQueryGeneratorPredicateConfiguration> CalibrationQueryGenerator::_generate_predicate_permutations() const {

  std::vector<CalibrationQueryGeneratorPredicateConfiguration> output {};

  for (const auto& encoding : _configuration.encodings) {
    for (const auto& data_type : _configuration.data_types) {
      for (const auto& selectivity : _configuration.selectivities) {
        for (const auto& table_name : _table_names) {
          output.push_back({table_name, encoding, data_type, selectivity, false});
          output.push_back({table_name, encoding, data_type, selectivity, true});
        }
      }
    }
  }

  return output;
}

const std::vector<std::shared_ptr<AbstractLQPNode>> CalibrationQueryGenerator::generate_queries() const {
  std::vector<std::shared_ptr<AbstractLQPNode>> queries;

  const auto& add_query_if_present = [](std::vector<std::shared_ptr<AbstractLQPNode>>& vector,
                                        const std::shared_ptr<AbstractLQPNode>& query) {
    if (query) {
      vector.push_back(query);
    }
  };


  for (const auto& table_name : _table_names) {
    add_query_if_present(queries, _generate_aggregate(table_name));
  }


  const auto permutations = _generate_predicate_permutations();
  for (const auto& permutation : permutations) {
      add_query_if_present(queries,
                           _generate_table_scan(permutation, CalibrationQueryGeneratorPredicate::generate_predicate_column_value));

      add_query_if_present(queries,
                           _generate_table_scan(permutation, CalibrationQueryGeneratorPredicate::generate_predicate_column_column));

      add_query_if_present(
              queries, _generate_table_scan(permutation, CalibrationQueryGeneratorPredicate::generate_predicate_between_value_value));

      add_query_if_present(
              queries, _generate_table_scan(permutation, CalibrationQueryGeneratorPredicate::generate_predicate_between_column_column));

      add_query_if_present(queries, _generate_table_scan(permutation, CalibrationQueryGeneratorPredicate::generate_predicate_like));

      add_query_if_present(queries, _generate_table_scan(permutation, CalibrationQueryGeneratorPredicate::generate_predicate_or));

      add_query_if_present(queries,
                           _generate_table_scan(permutation, CalibrationQueryGeneratorPredicate::generate_predicate_equi_on_strings));
  }


  for (const auto& left_table_name : _table_names) {
    for (const auto& right_table_name : _table_names) {
      CalibrationQueryGeneratorJoinConfiguration join_configuration{left_table_name, right_table_name, EncodingType::Unencoded, DataType::Int, false};
      // Generates the same query using all available JoinTypes
      const auto& join_queries = _generate_join(join_configuration, left_table_name, right_table_name);
      for (const auto& query : join_queries) {
        add_query_if_present(queries, query);
      }
    }
  }

  return queries;
}

//const std::shared_ptr<MockNode> CalibrationQueryGenerator::_column_specification_to_mock_node() const {
//  std::vector<std::pair<DataType, std::string>> column_definitions;
//  for (const auto& column : _column_specifications) {
//    column_definitions.emplace_back(column.type, column.column_name);
//  }
//
//  return MockNode::make(column_definitions);
//}

const std::shared_ptr<AbstractLQPNode> CalibrationQueryGenerator::_generate_table_scan(
        const CalibrationQueryGeneratorPredicateConfiguration& configuration,
    const PredicateGeneratorFunctor& predicate_generator) const {
  const auto table = StoredTableNode::make(configuration.table_name);

//  CalibrationQueryGeneratorPredicateConfiguration configuration{EncodingType::Unencoded, DataType::String, 0.1f, false};

  auto predicate = CalibrationQueryGeneratorPredicate::generate_predicates(predicate_generator, _column_specifications,
                                                                           table, configuration);
  // TODO(Sven): Add test
  if (!predicate) return {};

  // TODO(Sven): add ValidateNode if asked for it

  if (configuration.reference_column) {
    const auto validate_node = ValidateNode::make();
    validate_node->set_left_input(table);
    predicate->set_left_input(validate_node);
  } else {
    predicate->set_left_input(table);
  }

  return predicate;
}

const std::vector<std::shared_ptr<AbstractLQPNode>> CalibrationQueryGenerator::_generate_join(
        const CalibrationQueryGeneratorJoinConfiguration& configuration,
        const std::string& left_table_name, const std::string& right_table_name) const {
  const auto left_table = StoredTableNode::make(left_table_name);
  const auto right_table = StoredTableNode::make(right_table_name);

  const auto join_nodes = CalibrationQueryGeneratorJoin::generate_join(
      configuration, CalibrationQueryGeneratorJoin::generate_join_predicate, left_table, right_table, _column_specifications);

  if (join_nodes.empty()) {
    return {};
  }

//  CalibrationQueryGeneratorPredicateConfiguration left_configuration{left_table_name, EncodingType::Unencoded, DataType::Int, 0.1f, false};
//  CalibrationQueryGeneratorPredicateConfiguration right_configuration{right_table_name, EncodingType::Unencoded, DataType::Int, 0.1f, false};
//
//  auto left_predicate = CalibrationQueryGeneratorPredicate::generate_predicates(
//      CalibrationQueryGeneratorPredicate::generate_predicate_column_value, _column_specifications, left_table,
//      left_configuration);
//
//  auto right_predicate = CalibrationQueryGeneratorPredicate::generate_predicates(
//      CalibrationQueryGeneratorPredicate::generate_predicate_column_value, _column_specifications, right_table,
//      right_configuration);
//
//   We are not interested in queries without Predicates
//  if (!left_predicate || !right_predicate) {
//    return {};
//  }

//  left_predicate->set_left_input(left_table);
//  right_predicate->set_left_input(right_table);


//  if (configuration.reference_column) {
//    const auto validate_node = ValidateNode::make();
//    validate_node->set_left_input(table);
//    predicate->set_left_input(validate_node);
//  } else {
//    predicate->set_left_input(table);
//  }

//  return predicate;


  for (const auto& join_node : join_nodes) {
    if (configuration.reference_column) {
      const auto left_validate_node = ValidateNode::make();
      const auto right_validate_node = ValidateNode::make();

      left_validate_node->set_left_input(left_table);
      right_validate_node->set_left_input(right_table);

      join_node->set_left_input(left_validate_node);
      join_node->set_right_input(right_validate_node);

    } else {
      join_node->set_left_input(left_table);
      join_node->set_right_input(right_table);
    }
  }

  return join_nodes;
}

const std::shared_ptr<AbstractLQPNode> CalibrationQueryGenerator::_generate_aggregate(const std::string& table_name) const {
  const auto table = StoredTableNode::make(table_name);

  const auto aggregate_node = CalibrationQueryGeneratorAggregate::generate_aggregates();

//  CalibrationQueryGeneratorPredicateConfiguration configuration{EncodingType::Unencoded, DataType::String, 0.1f, false};
//
//  auto predicate = CalibrationQueryGeneratorPredicate::generate_predicates(
//      CalibrationQueryGeneratorPredicate::generate_predicate_column_value, _column_specifications, table,
//      configuration);
//
//  if (!predicate) {
//    std::cout << "Failed to generate predicate for Aggregate" << std::endl;
//    return {};
//  }

//  predicate->set_left_input(table);
  aggregate_node->set_left_input(table);
  return aggregate_node;
}

const std::shared_ptr<ProjectionNode> CalibrationQueryGenerator::_generate_projection(
    const std::vector<LQPColumnReference>& columns) const {
  return CalibrationQueryGeneratorProjection::generate_projection(columns);
}
}  // namespace opossum