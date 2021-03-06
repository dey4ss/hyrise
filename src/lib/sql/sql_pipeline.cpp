#include "sql_pipeline.hpp"

#include <algorithm>
#include <utility>

#include <boost/algorithm/string.hpp>

#include "SQLParser.h"
#include "create_sql_parser_error_message.hpp"
#include "sql_plan_cache.hpp"
#include "utils/assert.hpp"
#include "utils/format_duration.hpp"
#include "utils/tracing/probes.hpp"

namespace opossum {

SQLPipeline::SQLPipeline(const std::string& sql, const std::shared_ptr<TransactionContext>& transaction_context,
                         const UseMvcc use_mvcc, const std::shared_ptr<Optimizer>& optimizer,
                         const std::shared_ptr<SQLPhysicalPlanCache>& pqp_cache,
                         const std::shared_ptr<SQLLogicalPlanCache>& lqp_cache,
                         const CleanupTemporaries cleanup_temporaries)
    : pqp_cache(pqp_cache),
      lqp_cache(lqp_cache),
      _sql(sql),
      _transaction_context(transaction_context),
      _optimizer(optimizer) {
  DebugAssert(!_transaction_context || _transaction_context->phase() == TransactionPhase::Active,
              "The transaction context cannot have been committed already.");
  DebugAssert(!_transaction_context || use_mvcc == UseMvcc::Yes,
              "Transaction context without MVCC enabled makes no sense");

  hsql::SQLParserResult parse_result;

  const auto start = std::chrono::high_resolution_clock::now();
  hsql::SQLParser::parse(sql, &parse_result);

  const auto done = std::chrono::high_resolution_clock::now();
  _metrics.parse_time_nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(done - start);
  DTRACE_PROBE2(HYRISE, SQL_PARSING, sql.c_str(), _metrics.parse_time_nanos.count());

  AssertInput(parse_result.isValid(), create_sql_parser_error_message(sql, parse_result));
  DebugAssert(parse_result.size() > 0, "Cannot create empty SQLPipeline.");

  _sql_pipeline_statements.reserve(parse_result.size());

  std::vector<std::shared_ptr<hsql::SQLParserResult>> parsed_statements;
  for (auto* statement : parse_result.releaseStatements()) {
    parsed_statements.emplace_back(std::make_shared<hsql::SQLParserResult>(statement));
  }

  auto seen_altering_statement = false;

  // We want to split the (multi-) statement SQL string into the strings for each statement. We can then use those
  // statement strings to cache query plans.
  // The sql parser only offers us the length of the string, so we need to split it manually.
  auto sql_string_offset = 0u;

  for (auto& parsed_statement : parsed_statements) {
    parsed_statement->setIsValid(true);

    // We will always have one at 0 because we set it ourselves
    const auto* statement = parsed_statement->getStatement(0);

    switch (statement->type()) {
      // Check if statement alters the structure of the database in a way that following statements might depend upon.
      case hsql::StatementType::kStmtImport:
      case hsql::StatementType::kStmtCreate:
      case hsql::StatementType::kStmtDrop:
      case hsql::StatementType::kStmtAlter:
      case hsql::StatementType::kStmtRename: {
        seen_altering_statement = true;
        break;
      }
      default: { /* do nothing */
      }
    }

    // Get the statement string from the original query string, so we can pass it to the SQLPipelineStatement
    const auto statement_string_length = statement->stringLength;
    const auto statement_string = boost::trim_copy(sql.substr(sql_string_offset, statement_string_length));
    sql_string_offset += statement_string_length;

    auto pipeline_statement = std::make_shared<SQLPipelineStatement>(statement_string, std::move(parsed_statement),
                                                                     use_mvcc, transaction_context, optimizer,
                                                                     pqp_cache, lqp_cache, cleanup_temporaries);
    _sql_pipeline_statements.push_back(std::move(pipeline_statement));
  }

  // If we see at least one structure altering statement and we have more than one statement, we require execution of a
  // statement before the next one can be translated (so the next statement sees the previous structural changes).
  _requires_execution = seen_altering_statement && statement_count() > 1;
}

const std::string& SQLPipeline::get_sql() const { return _sql; }

const std::vector<std::string>& SQLPipeline::get_sql_per_statement() {
  if (!_sql_strings.empty()) {
    return _sql_strings;
  }

  _sql_strings.reserve(statement_count());
  for (auto& pipeline_statement : _sql_pipeline_statements) {
    _sql_strings.push_back(pipeline_statement->get_sql_string());
  }

  return _sql_strings;
}

const std::vector<std::shared_ptr<hsql::SQLParserResult>>& SQLPipeline::get_parsed_sql_statements() {
  if (!_parsed_sql_statements.empty()) {
    return _parsed_sql_statements;
  }

  _parsed_sql_statements.reserve(statement_count());
  for (auto& pipeline_statement : _sql_pipeline_statements) {
    _parsed_sql_statements.push_back(pipeline_statement->get_parsed_sql_statement());
  }

  return _parsed_sql_statements;
}

const std::vector<std::shared_ptr<AbstractLQPNode>>& SQLPipeline::get_unoptimized_logical_plans() {
  if (!_unoptimized_logical_plans.empty()) {
    return _unoptimized_logical_plans;
  }

  Assert(!_requires_execution || _pipeline_status != SQLPipelineStatus::NotExecuted,
         "One or more SQL statement is dependent on the execution of a previous one. "
         "Cannot translate all statements without executing, i.e. calling get_result_table()");

  _unoptimized_logical_plans.reserve(statement_count());
  for (auto& pipeline_statement : _sql_pipeline_statements) {
    _unoptimized_logical_plans.push_back(pipeline_statement->get_unoptimized_logical_plan());
  }

  return _unoptimized_logical_plans;
}

const std::vector<std::shared_ptr<AbstractLQPNode>>& SQLPipeline::get_optimized_logical_plans() {
  if (!_optimized_logical_plans.empty()) {
    return _optimized_logical_plans;
  }

  Assert(!_requires_execution || _pipeline_status != SQLPipelineStatus::NotExecuted,
         "One or more SQL statement is dependent on the execution of a previous one. "
         "Cannot translate all statements without executing, i.e. calling get_result_table()");

  // The optimizer modifies the input LQP and requires exclusive ownership of that LQP. This means that we need to
  // clear _unoptimized_logical_plans. This is not an issue as the unoptimized plans will no longer be needed.
  // Calls to get_unoptimized_logical_plans are still allowed (e.g., for visualization), in which case the unoptimized
  // plan will be recreated. Note that this
  // does not clear the unoptimized LQPs stored in the SQLPipelineStatement - those are cleared as part of
  // SQLPipelineStatement::get_optimized_logical_plan.
  _unoptimized_logical_plans.clear();

  _optimized_logical_plans.reserve(statement_count());
  for (auto& pipeline_statement : _sql_pipeline_statements) {
    _optimized_logical_plans.push_back(pipeline_statement->get_optimized_logical_plan());
  }

  return _optimized_logical_plans;
}

const std::vector<std::shared_ptr<AbstractOperator>>& SQLPipeline::get_physical_plans() {
  if (!_physical_plans.empty()) {
    return _physical_plans;
  }

  Assert(!_requires_execution || _pipeline_status != SQLPipelineStatus::NotExecuted,
         "One or more SQL statement is dependent on the execution of a previous one. "
         "Cannot compile all statements without executing, i.e. calling get_result_table()");

  _physical_plans.reserve(statement_count());
  for (auto& pipeline_statement : _sql_pipeline_statements) {
    _physical_plans.push_back(pipeline_statement->get_physical_plan());
  }

  return _physical_plans;
}

const std::vector<std::vector<std::shared_ptr<OperatorTask>>>& SQLPipeline::get_tasks() {
  if (!_tasks.empty()) {
    return _tasks;
  }

  Assert(!_requires_execution || _pipeline_status != SQLPipelineStatus::NotExecuted,
         "One or more SQL statement is dependent on the execution of a previous one. "
         "Cannot generate tasks for all statements without executing, i.e. calling get_result_table()");

  _tasks.reserve(statement_count());
  for (auto& pipeline_statement : _sql_pipeline_statements) {
    _tasks.push_back(pipeline_statement->get_tasks());
  }

  return _tasks;
}

std::pair<SQLPipelineStatus, const std::shared_ptr<const Table>&> SQLPipeline::get_result_table() & {
  const auto& [pipeline_status, tables] = get_result_tables();

  if (pipeline_status != SQLPipelineStatus::Success) {
    static std::shared_ptr<const Table> null_table;
    return {pipeline_status, null_table};
  }

  return {SQLPipelineStatus::Success, tables.back()};
}

std::pair<SQLPipelineStatus, std::shared_ptr<const Table>> SQLPipeline::get_result_table() && {
  // std::pair constructor will automatically convert the reference into a copy
  return get_result_table();
}

std::pair<SQLPipelineStatus, const std::vector<std::shared_ptr<const Table>>&> SQLPipeline::get_result_tables() & {
  if (_pipeline_status != SQLPipelineStatus::NotExecuted) {
    return {_pipeline_status, _result_tables};
  }

  _result_tables.reserve(_sql_pipeline_statements.size());

  for (auto& pipeline_statement : _sql_pipeline_statements) {
    const auto& [statement_status, table] = pipeline_statement->get_result_table();
    if (statement_status == SQLPipelineStatus::RolledBack) {
      _failed_pipeline_statement = pipeline_statement;

      if (_transaction_context) {
        // The pipeline was executed using a transaction context (i.e., no auto-commit after each statement).
        // Previously returned results are invalid.
        _result_tables.clear();
      }

      return {SQLPipelineStatus::RolledBack, _result_tables};
    }

    DebugAssert(statement_status == SQLPipelineStatus::Success, "Unexpected pipeline status");

    _result_tables.emplace_back(table);
  }

  _pipeline_status = SQLPipelineStatus::Success;
  return {_pipeline_status, _result_tables};
}

std::pair<SQLPipelineStatus, std::vector<std::shared_ptr<const Table>>> SQLPipeline::get_result_tables() && {
  // std::pair constructor will automatically convert the reference into a copy
  return get_result_tables();
}

std::shared_ptr<TransactionContext> SQLPipeline::transaction_context() const { return _transaction_context; }

std::shared_ptr<SQLPipelineStatement> SQLPipeline::failed_pipeline_statement() const {
  return _failed_pipeline_statement;
}

size_t SQLPipeline::statement_count() const { return _sql_pipeline_statements.size(); }

bool SQLPipeline::requires_execution() const { return _requires_execution; }

SQLPipelineMetrics& SQLPipeline::metrics() {
  if (_metrics.statement_metrics.empty()) {
    _metrics.statement_metrics.reserve(statement_count());
    for (const auto& pipeline_statement : _sql_pipeline_statements) {
      _metrics.statement_metrics.push_back(pipeline_statement->metrics());
    }
  }

  return _metrics;
}

std::ostream& operator<<(std::ostream& stream, const SQLPipelineMetrics& metrics) {
  auto total_sql_translate_nanos = std::chrono::nanoseconds::zero();
  auto total_optimize_nanos = std::chrono::nanoseconds::zero();
  auto total_lqp_translate_nanos = std::chrono::nanoseconds::zero();
  auto total_execute_nanos = std::chrono::nanoseconds::zero();
  std::vector<bool> query_plan_cache_hits;

  for (const auto& statement_metric : metrics.statement_metrics) {
    total_sql_translate_nanos += statement_metric->sql_translation_duration;
    total_optimize_nanos += statement_metric->optimization_duration;
    total_lqp_translate_nanos += statement_metric->lqp_translation_duration;
    total_execute_nanos += statement_metric->plan_execution_duration;

    query_plan_cache_hits.push_back(statement_metric->query_plan_cache_hit);
  }

  const auto num_cache_hits = std::count(query_plan_cache_hits.begin(), query_plan_cache_hits.end(), true);

  stream << "Execution info: [";
  stream << "PARSE: " << format_duration(metrics.parse_time_nanos) << ", ";
  stream << "SQL TRANSLATE: " << format_duration(total_sql_translate_nanos) << ", ";
  stream << "OPTIMIZE: " << format_duration(total_optimize_nanos) << ", ";
  stream << "LQP TRANSLATE: " << format_duration(total_lqp_translate_nanos) << ", ";
  stream << "EXECUTE: " << format_duration(total_execute_nanos) << " (wall time) | ";
  stream << "QUERY PLAN CACHE HITS: " << num_cache_hits << "/" << query_plan_cache_hits.size() << " statement(s)";
  stream << "]\n";

  return stream;
}

}  // namespace opossum
