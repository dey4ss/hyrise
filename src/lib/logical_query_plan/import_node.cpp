#include "import_node.hpp"

#include <sstream>

#include "constant_mappings.hpp"
#include "static_table_node.hpp"

namespace opossum {

ImportNode::ImportNode(const std::string& table_name, const std::string& file_path, const hsql::ImportType file_type)
    : BaseNonQueryNode(LQPNodeType::Import), table_name(table_name), file_path(file_path), file_type(file_type) {}

std::string ImportNode::description() const {
  std::ostringstream stream;
  stream << "[Import] Name: '" << table_name << "'";
  return stream.str();
}

size_t ImportNode::_shallow_hash() const {
  auto hash = boost::hash_value(table_name);
  boost::hash_combine(hash, file_path);
  boost::hash_combine(hash, file_type);
  return hash;
}

std::shared_ptr<AbstractLQPNode> ImportNode::_on_shallow_copy(LQPNodeMapping& node_mapping) const {
  return ImportNode::make(table_name, file_path, file_type, left_input());
}

bool ImportNode::_on_shallow_equals(const AbstractLQPNode& rhs, const LQPNodeMapping& node_mapping) const {
  const auto& import_node = static_cast<const ImportNode&>(rhs);
  return table_name == import_node.table_name
          && file_path == import_node.file_path
          && file_type == import_node.file_type;
}

}  // namespace opossum
