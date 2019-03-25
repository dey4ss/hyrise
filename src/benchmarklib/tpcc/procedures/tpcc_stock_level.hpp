#include <boost/container/small_vector.hpp>

#include "abstract_tpcc_procedure.hpp"

namespace opossum {

class TpccStockLevel : public AbstractTpccProcedure {
public:
  TpccStockLevel(const int num_warehouses);

  void execute() override;
  std::ostream& print(std::ostream& stream = std::cout) const override;

protected:
  // Values generated BEFORE the procedure is executed:
  int32_t _w_id;        // Home warehouse ID    [1..num_warehouses]
  int32_t _d_id;        // District ID          [1..10]
  int32_t _threshold;   // Minimum stock level  [10..20]
};

}
