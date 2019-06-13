#include <ctime>
#include <random>

#include "tpcc_stock_level.hpp"

#include "operators/print.hpp"

namespace opossum {

TpccStockLevel::TpccStockLevel(const int num_warehouses, BenchmarkSQLExecutor sql_executor)
    : AbstractTpccProcedure(sql_executor) {
  std::uniform_int_distribution<> warehouse_dist{1, num_warehouses};
  _w_id = warehouse_dist(_random_engine);

  std::uniform_int_distribution<> district_dist{1, 10};
  _d_id = district_dist(_random_engine);

  std::uniform_int_distribution<> threshold_dist{10, 20};
  _threshold = threshold_dist(_random_engine);
}

bool TpccStockLevel::execute() {
  const auto district_table_pair =
      _sql_executor.execute(std::string{"SELECT D_NEXT_O_ID FROM DISTRICT WHERE D_W_ID = "} + std::to_string(_w_id) +
                            " AND D_ID = " + std::to_string(_d_id));
  const auto& district_table = district_table_pair.second;
  Assert(district_table->row_count() == 1, "Did not find district (or found more than one)");
  auto first_o_id = district_table->get_value<int32_t>(ColumnID{0}, 0) - 20;

  const auto order_line_table_pair = _sql_executor.execute(
      std::string{"SELECT OL_I_ID FROM ORDER_LINE WHERE OL_W_ID = "} + std::to_string(_w_id) +
      " AND OL_D_ID = " + std::to_string(_d_id) + " AND OL_O_ID >= " + std::to_string(first_o_id));
  const auto& order_line_table = order_line_table_pair.second;
  Assert(order_line_table->row_count() > 0, "Did not find latest orders");

  std::stringstream ol_i_ids_stream;
  const auto NUM_ORDERS_PER_WAREHOUSE = static_cast<int>(order_line_table->row_count());
  for (auto order_line_idx = 0; order_line_idx < NUM_ORDERS_PER_WAREHOUSE; ++order_line_idx) {
    ol_i_ids_stream << order_line_table->get_value<int32_t>(ColumnID{0}, order_line_idx) << ", ";
  }
  auto ol_i_ids = ol_i_ids_stream.str();
  ol_i_ids.resize(ol_i_ids.size() - 2);  // Remove final ", "

  _sql_executor.execute(std::string{"SELECT COUNT(*) FROM STOCK WHERE S_I_ID IN ("} + ol_i_ids +
                        ") AND S_W_ID = " + std::to_string(_w_id) + " AND S_QUANTITY < " + std::to_string(_threshold));

  // No need to commit the transaction as we have not modified anything
  _sql_executor.transaction_context = nullptr;
  return true;
}

char TpccStockLevel::identifier() const { return 'S'; }

}  // namespace opossum
