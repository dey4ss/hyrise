#include "base_test.hpp"
#include "operators/get_table.hpp"
#include "operators/insert.hpp"
#include "operators/table_wrapper.hpp"
#include "operators/validate.hpp"
#include "tpcc/constants.hpp"
#include "tpcc/procedures/tpcc_delivery.hpp"
#include "tpcc/procedures/tpcc_new_order.hpp"
#include "tpcc/procedures/tpcc_order_status.hpp"
#include "tpcc/procedures/tpcc_payment.hpp"
#include "tpcc/tpcc_table_generator.hpp"

namespace opossum {

class TPCCTest : public BaseTest {
 public:
  static void SetUpTestCase() {
    auto benchmark_config = std::make_shared<BenchmarkConfig>(BenchmarkConfig::get_default_config());
    auto table_generator = TPCCTableGenerator{NUM_WAREHOUSES, benchmark_config};

    tables = table_generator.generate();
  }

  void SetUp() override {
    for (const auto& [table_name, table_info] : tables) {
      // Copy the data into a new table in order to isolate tests
      const auto generated_table = table_info.table;
      auto isolated_table =
          std::make_shared<Table>(generated_table->column_definitions(), TableType::Data, std::nullopt, UseMvcc::Yes);
      StorageManager::get().add_table(table_name, isolated_table);

      auto table_wrapper = std::make_shared<TableWrapper>(generated_table);
      table_wrapper->execute();
      auto insert = std::make_shared<Insert>(table_name, table_wrapper);
      auto transaction_context = TransactionManager::get().new_transaction_context();
      insert->set_transaction_context(transaction_context);
      insert->execute();
      transaction_context->commit();
    }
  }

  std::shared_ptr<const Table> get_validated(const std::string& table_name,
                                             const std::shared_ptr<TransactionContext>& transaction_context) {
    auto get_table = std::make_shared<GetTable>(table_name);
    get_table->execute();

    auto validate = std::make_shared<Validate>(get_table);
    validate->set_transaction_context(transaction_context);
    validate->execute();

    return validate->get_output();
  }

  void verify_table_sizes(const std::unordered_map<std::string, size_t>& sizes) {
    for (const auto& [table_name, size] : sizes) {
      auto transaction_context = TransactionManager::get().new_transaction_context();
      const auto table = get_validated(table_name, transaction_context);

      if (table_name == "ORDER_LINE" && size == 0) {
        // For ORDER_LINE, the number of lines per order is in [5, 15] and non-deterministic. If it is not specifically
        // calculated, we just check if it is within the acceptable range
        EXPECT_GE(table->row_count(), sizes.at("ORDER") * 5) << "Failed table: " << table_name;
        EXPECT_LE(table->row_count(), sizes.at("ORDER") * 15) << "Failed table: " << table_name;
      } else {
        EXPECT_EQ(table->row_count(), size) << "Failed table: " << table_name;
      }
    }
  }

  // See TPC-C Spec 1.2.1
  const std::unordered_map<std::string, size_t> initial_sizes{{"WAREHOUSE", NUM_WAREHOUSES},
                                                              {"DISTRICT", NUM_WAREHOUSES * 10},
                                                              {"HISTORY", NUM_WAREHOUSES * 30'000},
                                                              {"STOCK", NUM_WAREHOUSES * 100'000},
                                                              {"NEW_ORDER", NUM_WAREHOUSES * 9'000},
                                                              {"CUSTOMER", NUM_WAREHOUSES * 30'000},
                                                              {"ITEM", 100'000},
                                                              {"ORDER_LINE", 0},  // see verify_table_sizes
                                                              {"ORDER", NUM_WAREHOUSES * 30'000}};

  static std::unordered_map<std::string, BenchmarkTableInfo> tables;
  static constexpr auto NUM_WAREHOUSES = 2;
};

std::unordered_map<std::string, BenchmarkTableInfo> TPCCTest::tables;

TEST_F(TPCCTest, InitialTables) { verify_table_sizes(initial_sizes); }

TEST_F(TPCCTest, Delivery) {
  auto old_transaction_context = TransactionManager::get().new_transaction_context();
  const auto old_time = time(nullptr);

  BenchmarkSQLExecutor sql_executor{false, nullptr, std::nullopt};
  auto delivery = TPCCDelivery{NUM_WAREHOUSES, sql_executor};
  EXPECT_TRUE(delivery.execute());

  auto new_sizes = initial_sizes;
  new_sizes["NEW_ORDER"] -= 10;
  verify_table_sizes(new_sizes);

  // These tests assume that updates / insertions into a table are done in the same physical order as they were in the
  // original table
  for (auto d_id = 1; d_id <= 10; ++d_id) {
    // Store data as it was before the procedure was executed:
    uint64_t num_order_lines;
    float old_c_balance, expected_c_balance;
    int old_c_delivery_cnt;

    // Retrieve old data
    {
      // We have #ORDER (3000) - #NEW_ORDER (900) delivered orders per warehouse, so 2101 is the first undelivered ID
      auto pipeline =
          SQLPipelineBuilder{std::string{"SELECT * FROM ORDER_LINE WHERE OL_W_ID = "} + std::to_string(delivery.w_id) +
                             " AND OL_D_ID = " + std::to_string(d_id) + " AND OL_O_ID = 2101"}
              .with_transaction_context(old_transaction_context)
              .create_pipeline();
      const auto [_, table] = pipeline.get_result_table();
      EXPECT_TRUE(table);
      num_order_lines = table->row_count();
      EXPECT_GE(num_order_lines, 5);
      EXPECT_LE(num_order_lines, 15);
    }
    {
      auto pipeline = SQLPipelineBuilder{std::string{"SELECT * FROM CUSTOMER WHERE C_W_ID = "} +
                                         std::to_string(delivery.w_id) + " AND C_D_ID = " + std::to_string(d_id) +
                                         " AND C_ID = (SELECT O_C_ID FROM \"ORDER\" WHERE O_W_ID = " +
                                         std::to_string(delivery.w_id) + " AND O_D_ID = " + std::to_string(d_id) +
                                         " AND O_ID = 2101)"}
                          .with_transaction_context(old_transaction_context)
                          .create_pipeline();
      const auto [_, table] = pipeline.get_result_table();
      EXPECT_TRUE(table);
      EXPECT_EQ(table->row_count(), 1);
      old_c_balance = table->get_value<float>(ColumnID{16}, 0);
      old_c_delivery_cnt = table->get_value<int>(ColumnID{19}, 0);
    }
    expected_c_balance = old_c_balance;

    // Verify that the order was deleted from the NEW_ORDER table:
    {
      auto pipeline =
          SQLPipelineBuilder{std::string{"SELECT * FROM NEW_ORDER WHERE NO_W_ID = "} + std::to_string(delivery.w_id) +
                             " AND NO_D_ID = " + std::to_string(d_id) + " AND NO_O_ID = 2101"}
              .create_pipeline();
      const auto [_, table] = pipeline.get_result_table();
      EXPECT_TRUE(table);
      EXPECT_EQ(table->row_count(), 0);
    }

    int c_id;

    // Verify that it was updated in the ORDER table - not filtering by the district ID to keep it interesting:
    {
      auto pipeline = SQLPipelineBuilder{std::string{"SELECT * FROM \"ORDER\" WHERE O_W_ID = "} +
                                         std::to_string(delivery.w_id) + " AND O_ID = 2101"}
                          .create_pipeline();
      const auto [_, table] = pipeline.get_result_table();
      EXPECT_TRUE(table);
      EXPECT_EQ(table->row_count(), 10);
      EXPECT_EQ(table->get_value<int>(ColumnID{5}, d_id - 1), delivery.o_carrier_id);  // O_CARRIER_ID

      c_id = table->get_value<int>(ColumnID{3}, d_id - 1);
    }

    // Verify the entries in ORDER_LINE:
    {
      auto pipeline =
          SQLPipelineBuilder{std::string{"SELECT * FROM ORDER_LINE WHERE OL_W_ID = "} + std::to_string(delivery.w_id) +
                             " AND OL_D_ID = " + std::to_string(d_id) + " AND OL_O_ID = 2101"}
              .create_pipeline();
      const auto [_, table] = pipeline.get_result_table();
      EXPECT_TRUE(table);
      EXPECT_EQ(table->row_count(), num_order_lines);
      for (auto ol_number = uint64_t{1}; ol_number <= table->row_count(); ++ol_number) {
        EXPECT_EQ(table->get_value<int>(ColumnID{3}, ol_number - 1), ol_number);  // OL_NUMBER
        EXPECT_GE(table->get_value<int>(ColumnID{6}, ol_number - 1), old_time);   // OL_DELIVERY_D
        expected_c_balance += table->get_value<float>(ColumnID{8}, ol_number - 1);
      }
    }

    // Verify the entry in CUSTOMER:
    {
      auto pipeline = SQLPipelineBuilder{std::string{"SELECT C_BALANCE, C_DELIVERY_CNT FROM CUSTOMER WHERE C_W_ID = "} +
                                         std::to_string(delivery.w_id) + " AND C_D_ID = " + std::to_string(d_id) +
                                         " AND C_ID = " + std::to_string(c_id)}
                          .create_pipeline();
      const auto [_, table] = pipeline.get_result_table();
      EXPECT_TRUE(table);
      EXPECT_EQ(table->row_count(), 1);
      EXPECT_FLOAT_EQ(table->get_value<float>(ColumnID{0}, 0), expected_c_balance);
      EXPECT_EQ(table->get_value<int>(ColumnID{1}, 0), old_c_delivery_cnt + 1);
    }
  }
}

TEST_F(TPCCTest, NewOrder) {
  auto old_transaction_context = TransactionManager::get().new_transaction_context();
  const auto old_order_line_size = static_cast<int>(StorageManager::get().get_table("ORDER_LINE")->row_count());
  const auto old_time = time(nullptr);

  BenchmarkSQLExecutor sql_executor{false, nullptr, std::nullopt};
  auto new_order = TPCCNewOrder{NUM_WAREHOUSES, sql_executor};
  // Generate random NewOrders until we have one without invalid item IDs and where both local and remote order lines
  // occur
  while (new_order.order_lines.back().ol_i_id == TPCCNewOrder::INVALID_ITEM_ID ||
         std::find_if(new_order.order_lines.begin(), new_order.order_lines.end(), [&](const auto& order_line) {
           return order_line.ol_supply_w_id != new_order.w_id;
         }) == new_order.order_lines.end()) {
    new_order = TPCCNewOrder{NUM_WAREHOUSES, sql_executor};
  }

  const auto& order_lines = new_order.order_lines;
  EXPECT_GE(order_lines.size(), 5);
  EXPECT_LE(order_lines.size(), 15);

  EXPECT_TRUE(new_order.execute());

  int old_d_next_o_id;
  {
    auto pipeline = SQLPipelineBuilder{std::string{"SELECT D_NEXT_O_ID FROM DISTRICT WHERE D_W_ID = "} +
                                       std::to_string(new_order.w_id) + " AND D_ID = " + std::to_string(new_order.d_id)}
                        .with_transaction_context(old_transaction_context)
                        .create_pipeline();
    const auto [_, table] = pipeline.get_result_table();
    EXPECT_TRUE(table);
    EXPECT_EQ(table->row_count(), 1);
    old_d_next_o_id = table->get_value<int>(ColumnID{0}, 0);
  }

  // Verify updated D_NEXT_O_ID
  {
    auto pipeline = SQLPipelineBuilder{std::string{"SELECT D_NEXT_O_ID FROM DISTRICT WHERE D_W_ID = "} +
                                       std::to_string(new_order.w_id) + " AND D_ID = " + std::to_string(new_order.d_id)}
                        .create_pipeline();
    const auto [_, table] = pipeline.get_result_table();
    EXPECT_TRUE(table);
    EXPECT_EQ(table->row_count(), 1);
    EXPECT_EQ(table->get_value<int>(ColumnID{0}, 0), old_d_next_o_id + 1);
  }

  auto new_sizes = initial_sizes;
  new_sizes["NEW_ORDER"] += 1;
  new_sizes["ORDER"] += 1;
  new_sizes["ORDER_LINE"] = old_order_line_size + order_lines.size();
  verify_table_sizes(new_sizes);

  // Verify NEW_ORDER entry
  const auto new_order_row = StorageManager::get().get_table("NEW_ORDER")->get_row(new_sizes["NEW_ORDER"] - 1);
  EXPECT_EQ(new_order_row[0], AllTypeVariant{NUM_ORDERS_PER_DISTRICT + 1});
  EXPECT_EQ(new_order_row[1], AllTypeVariant{new_order.d_id});
  EXPECT_EQ(new_order_row[2], AllTypeVariant{new_order.w_id});

  // Verify ORDER entry
  const auto order_row = StorageManager::get().get_table("ORDER")->get_row(new_sizes["ORDER"] - 1);
  EXPECT_EQ(order_row[0], AllTypeVariant{NUM_ORDERS_PER_DISTRICT + 1});
  EXPECT_EQ(order_row[1], AllTypeVariant{new_order.d_id});
  EXPECT_EQ(order_row[2], AllTypeVariant{new_order.w_id});
  EXPECT_EQ(order_row[3], AllTypeVariant{new_order.c_id});
  EXPECT_GE(order_row[4], AllTypeVariant{static_cast<int32_t>(old_time)});
  EXPECT_EQ(order_row[5], AllTypeVariant{-1});  // TODO(anyone): Replace with actual NULL for O_CARRIER_ID
  EXPECT_EQ(order_row[6], AllTypeVariant{static_cast<int32_t>(order_lines.size())});
  EXPECT_EQ(order_row[7], AllTypeVariant{0});

  // VERIFY ORDER_LINE entries
  for (auto line_idx = size_t{0}; line_idx < order_lines.size(); ++line_idx) {
    const auto row_idx = new_sizes["ORDER_LINE"] - order_lines.size() + line_idx;
    const auto order_line_row = StorageManager::get().get_table("ORDER_LINE")->get_row(row_idx);
    EXPECT_EQ(order_line_row[0], AllTypeVariant{NUM_ORDERS_PER_DISTRICT + 1});
    EXPECT_EQ(order_line_row[1], AllTypeVariant{new_order.d_id});
    EXPECT_EQ(order_line_row[2], AllTypeVariant{new_order.w_id});
    EXPECT_EQ(order_line_row[3], AllTypeVariant{static_cast<int32_t>(line_idx + 1)});
    EXPECT_EQ(order_line_row[4], AllTypeVariant{order_lines[line_idx].ol_i_id});
    EXPECT_LE(order_lines[line_idx].ol_i_id, NUM_ITEMS);
    EXPECT_LE(order_line_row[5], AllTypeVariant{NUM_WAREHOUSES});
    EXPECT_EQ(order_line_row[6], AllTypeVariant{-1});
    EXPECT_EQ(order_line_row[7], AllTypeVariant{order_lines[line_idx].ol_quantity});
  }
}

TEST_F(TPCCTest, NewOrderUnusedOrderId) {
  BenchmarkSQLExecutor sql_executor{false, nullptr, std::nullopt};
  auto new_order = TPCCNewOrder{NUM_WAREHOUSES, sql_executor};
  // Generate random NewOrders until we have one with an invalid item ID
  while (new_order.order_lines.back().ol_i_id != TPCCNewOrder::INVALID_ITEM_ID) {
    new_order = TPCCNewOrder{NUM_WAREHOUSES, sql_executor};
  }

  const auto& order_lines = new_order.order_lines;
  EXPECT_GE(order_lines.size(), 5);
  EXPECT_LE(order_lines.size(), 15);

  // TPC-C transactions with simulated user input errors are still counted as successful
  EXPECT_TRUE(new_order.execute());

  auto new_transaction_context = TransactionManager::get().new_transaction_context();

  // None of the tables should have been visibly modified
  for (const auto& [table_name, table_info] : tables) {
    auto get_table = std::make_shared<GetTable>(table_name);
    get_table->execute();
    auto validate = std::make_shared<Validate>(get_table);
    validate->set_transaction_context(new_transaction_context);
    validate->execute();

    EXPECT_TABLE_EQ_UNORDERED(validate->get_output(), table_info.table);
  }
}

TEST_F(TPCCTest, PaymentCustomerByName) {
  const auto old_time = time(nullptr);

  BenchmarkSQLExecutor sql_executor{false, nullptr, std::nullopt};
  auto payment = TPCCPayment{NUM_WAREHOUSES, sql_executor};
  // Generate random payments until we have one that identified the customer by name
  while (!payment.select_customer_by_name) {
    payment = TPCCPayment{NUM_WAREHOUSES, sql_executor};
  }

  EXPECT_TRUE(payment.execute());

  // Verify that W_YTD is updated
  pmr_string w_name;
  {
    auto pipeline = SQLPipelineBuilder{std::string{"SELECT W_YTD, W_NAME FROM WAREHOUSE WHERE W_ID = "} +
                                       std::to_string(payment.w_id)}
                        .create_pipeline();
    const auto [_, table] = pipeline.get_result_table();
    EXPECT_TRUE(table);
    EXPECT_EQ(table->row_count(), 1);
    // As all warehouses start with W_YTD = 300'000, this should be the first and only change
    EXPECT_FLOAT_EQ(table->get_value<float>(ColumnID{0}, 0), 300'000.0f + payment.h_amount);
    w_name = table->get_value<pmr_string>(ColumnID{1}, 0);
  }

  // Verify that D_YTD is updated
  pmr_string d_name;
  {
    auto pipeline = SQLPipelineBuilder{std::string{"SELECT D_YTD, D_NAME FROM DISTRICT WHERE D_W_ID = "} +
                                       std::to_string(payment.w_id) + " AND D_ID = " + std::to_string(payment.d_id)}
                        .create_pipeline();
    const auto [_, table] = pipeline.get_result_table();
    EXPECT_TRUE(table);
    EXPECT_EQ(table->row_count(), 1);
    // As all districts start with D_YTD = 30'000, this should be the first and only change
    EXPECT_FLOAT_EQ(table->get_value<float>(ColumnID{0}, 0), 30'000.0f + payment.h_amount);
    d_name = table->get_value<pmr_string>(ColumnID{1}, 0);
  }

  // Verify that the customer is updated
  {
    auto pipeline = SQLPipelineBuilder{std::string{"SELECT C_BALANCE, C_YTD_PAYMENT, C_PAYMENT_CNT FROM CUSTOMER WHERE C_W_ID = "} +
                                       std::to_string(payment.w_id) + " AND C_D_ID = " + std::to_string(payment.d_id) + " AND C_ID = " + std::to_string(payment.c_id)}
                        .create_pipeline();
    const auto [_, table] = pipeline.get_result_table();
    EXPECT_TRUE(table);
    EXPECT_EQ(table->row_count(), 1);
    // Customers start with C_BALANCE = -10, C_YTD_PAYMENT = 10, C_PAYMENT_CNT = 1
    EXPECT_FLOAT_EQ(table->get_value<float>(ColumnID{0}, 0), -10.0f - payment.h_amount);
    EXPECT_FLOAT_EQ(table->get_value<float>(ColumnID{1}, 0), 10.0f + payment.h_amount);
    EXPECT_FLOAT_EQ(table->get_value<int>(ColumnID{2}, 0), 2);
  }

  // We do not test for C_DATA

  // Verify that a new history entry is written
  {
    auto pipeline = SQLPipelineBuilder{std::string{"SELECT H_DATE, H_AMOUNT, H_DATA FROM \"HISTORY\" WHERE H_W_ID = "} +
                                       std::to_string(payment.w_id) + " AND H_D_ID = " + std::to_string(payment.d_id) + " AND H_C_ID = " + std::to_string(payment.c_id) + " AND H_C_W_ID = " + std::to_string(payment.c_w_id) + " AND H_C_D_ID = " + std::to_string(payment.c_d_id)}
                        .create_pipeline();
    const auto [_, table] = pipeline.get_result_table();
    EXPECT_TRUE(table);
    EXPECT_GT(table->row_count(), 0);

    const auto row = table->row_count() - 1;
    EXPECT_GE(table->get_value<int>(ColumnID{0}, row), old_time);
    EXPECT_FLOAT_EQ(table->get_value<float>(ColumnID{1}, row), payment.h_amount);
    EXPECT_EQ(table->get_value<pmr_string>(ColumnID{2}, row), w_name + "    " + d_name);
  }
}

TEST_F(TPCCTest, OrderStatusCustomerById) {
  // We have covered customer selection by name in PaymentCustomerByName
  // As Order-Status has no externally visible changes, we create a new order and test for correct return values

  BenchmarkSQLExecutor sql_executor{false, nullptr, std::nullopt};

  auto new_order = TPCCNewOrder{NUM_WAREHOUSES, sql_executor};
  EXPECT_TRUE(new_order.execute());

  auto order_status = TPCCOrderStatus{NUM_WAREHOUSES, sql_executor};
  order_status.w_id = new_order.w_id;
  order_status.d_id = new_order.d_id;
  order_status.select_customer_by_name = false;
  order_status.customer = new_order.c_id;
  EXPECT_TRUE(order_status.execute());

  EXPECT_EQ(order_status.o_id, new_order.o_id);
  EXPECT_EQ(order_status.o_entry_d, new_order.o_entry_d);
  EXPECT_EQ(order_status.o_carrier_id, -1);

  int32_t ol_quantity_sum = 0;
  for (const auto& order_line : new_order.order_lines) {
    ol_quantity_sum += order_line.ol_quantity;
  }
  EXPECT_EQ(order_status.ol_quantity_sum, ol_quantity_sum);
}

TEST_F(TPCCTest, StockLevel) {
  // TODO
}

}  // namespace opossum
