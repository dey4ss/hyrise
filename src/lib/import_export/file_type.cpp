#include "file_type.hpp"

#include "utils/assert.hpp"

namespace opossum {

FileType import_type_to_file_type(const hsql::ImportType import_type) {
  switch (import_type) {
    case hsql::ImportType::kImportCSV:
      return FileType::Csv;
    case hsql::ImportType::kImportTbl:
      return FileType::Tbl;
  }
}

FileType file_type_from_filename(const std::string& filename) {
  const auto extension = std::string{std::filesystem::path{filename}.extension()};
  if (extension == ".csv") {
    return FileType::Csv;
  } else if (extension == ".tbl") {
    return FileType::Tbl;
  } else if (extension == ".bin") {
    return FileType::Binary;
  }
  Fail("Unknown file extension " + extension);
}

}  // namespace opossum
