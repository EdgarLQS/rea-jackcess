#include "rea_jackcess/database.hpp"

#include "util/sha256.hpp"

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

// Pinned PGeo sample kept outside version control; see plan/Jackcess-C++重写/04-P0基线核验记录.md.
constexpr const char* pinned_sample_sha256 =
    "0bc0acb9c732de1e17c43c2d0a6019f7b5a62ccf83c5818a1bc42db7dbc460c6";

void check(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

const rea_jackcess::Table& require_table(const std::vector<rea_jackcess::Table>& tables,
                                         const std::string& name) {
    const auto table = std::find_if(tables.begin(), tables.end(), [&](const auto& candidate) {
        return candidate.name() == name;
    });
    check(table != tables.end(), "real sample does not expose table " + name);
    return *table;
}

std::size_t column_index(const rea_jackcess::Table& table, const std::string& name) {
    const auto& columns = table.columns();
    for (std::size_t index = 0; index < columns.size(); ++index) {
        if (columns[index].name == name) {
            return index;
        }
    }
    throw std::runtime_error("real sample table is missing column " + name);
}

std::vector<bool> read_booleans(const rea_jackcess::Table& table,
                                std::size_t index,
                                std::size_t expected_rows) {
    std::vector<bool> values;
    auto cursor = table.rows();
    while (const auto row = cursor.next()) {
        values.push_back(std::get<bool>(row->at(index)));
    }
    check(values.size() == expected_rows,
          "real sample BOOLEAN column returned an unexpected row count");
    return values;
}

}

int main(int argc, char** argv) {
    try {
        if (argc != 2) {
            std::cerr << "Usage: rea_jackcess_real_sample_tests <sample.mdb>\n";
            return 2;
        }
        const std::filesystem::path path = argv[1];
        check(rea_jackcess::detail::sha256_file_hex(path) == pinned_sample_sha256,
              "real sample hash does not match the pinned PGeo fixture");

        const auto database = rea_jackcess::Database::open(path);
        const auto tables = database.tables();

        {
            const auto& geometry_columns = require_table(tables, "GDB_GeomColumns");
            check(geometry_columns.row_count_header() == 18,
                  "real sample GDB_GeomColumns changed its row count");
            const std::size_t has_z = column_index(geometry_columns, "HasZ");
            const std::size_t has_m = column_index(geometry_columns, "HasM");
            const auto& has_z_column = geometry_columns.columns()[has_z];
            const auto& has_m_column = geometry_columns.columns()[has_m];
            check(has_z_column.type == rea_jackcess::DataType::boolean && has_z_column.fixed &&
                      has_z_column.size == 1 && has_z_column.nullable == std::optional<bool>(false),
                  "real sample HasZ is not a required one-byte BOOLEAN column");
            check(has_m_column.type == rea_jackcess::DataType::boolean && has_m_column.fixed &&
                      has_m_column.size == 1 && has_m_column.nullable == std::optional<bool>(false),
                  "real sample HasM is not a required one-byte BOOLEAN column");

            std::vector<bool> expected_has_z(18, false);
            expected_has_z[15] = true;
            expected_has_z[16] = true;
            expected_has_z[17] = true;
            check(read_booleans(geometry_columns, has_z, 18) == expected_has_z,
                  "real sample HasZ BOOLEAN bits changed");
            check(read_booleans(geometry_columns, has_m, 18) == std::vector<bool>(18, false),
                  "real sample HasM BOOLEAN bits changed");
        }

        {
            const auto& aces = require_table(tables, "MSysACEs");
            check(aces.row_count_header() == 102, "real sample MSysACEs changed its row count");
            const std::size_t inheritable = column_index(aces, "FInheritable");
            const auto& column = aces.columns()[inheritable];
            check(column.type == rea_jackcess::DataType::boolean && column.fixed && column.size == 1 &&
                      column.nullable == std::optional<bool>(true),
                  "real sample FInheritable is not a nullable one-byte BOOLEAN column");
            const auto values = read_booleans(aces, inheritable, 102);
            check(static_cast<std::size_t>(std::count(values.begin(), values.end(), true)) == 4,
                  "real sample FInheritable BOOLEAN bit count changed");
        }

        {
            // A real ArcGIS/Access-created index: GDB_ItemTypes.ObjectID is a single-column
            // ascending LONG index, so seeks and ranges must match the full index scan.
            const auto& item_types = require_table(tables, "GDB_ItemTypes");
            check(item_types.row_count_header() == 34,
                  "real sample GDB_ItemTypes changed its row count");
            const std::size_t object_id = column_index(item_types, "ObjectID");

            std::vector<std::int32_t> keys;
            auto cursor = item_types.rows_by_index("FDO_ObjectID");
            while (const auto row = cursor.next()) {
                keys.push_back(std::get<std::int32_t>(row->at(object_id)));
            }
            check(keys.size() == 34 && std::is_sorted(keys.begin(), keys.end()),
                  "real sample FDO_ObjectID is not a sorted 34-entry index");

            const auto seek_from = [&](std::int32_t start) {
                std::vector<std::int32_t> values;
                auto seek = item_types.rows_by_index_from("FDO_ObjectID", start);
                while (const auto row = seek.next()) {
                    values.push_back(std::get<std::int32_t>(row->at(object_id)));
                }
                return values;
            };
            const auto suffix = [&](std::int32_t start) {
                const auto first = std::lower_bound(keys.begin(), keys.end(), start);
                return std::vector<std::int32_t>(first, keys.end());
            };
            check(seek_from(keys.front()) == keys,
                  "real sample seek from the first key did not return the whole index");
            check(seek_from(18) == suffix(18),
                  "real sample seek did not match the index suffix");
            check(seek_from(keys.back() + 1).empty(),
                  "real sample seek past the last key returned rows");

            std::vector<std::int32_t> range;
            auto ranged = item_types.rows_by_index_range("FDO_ObjectID", 5, 9);
            while (const auto row = ranged.next()) {
                range.push_back(std::get<std::int32_t>(row->at(object_id)));
            }
            check(range == std::vector<std::int32_t>({5, 6, 7, 8}),
                  "real sample range [5,9) did not return 5..8");

            const auto hit = item_types.find_row_by_index("FDO_ObjectID", 18);
            check(hit && std::get<std::int32_t>(hit->at(object_id)) == 18,
                  "real sample exact index lookup missed an existing key");
            check(!item_types.find_row_by_index("FDO_ObjectID", keys.back() + 1),
                  "real sample exact index lookup matched a missing key");

            const auto label = column_index(item_types, "Name");
            auto text_scan = item_types.rows_by_index("CatItemTypesByName");
            const auto first_label = text_scan.next();
            check(first_label.has_value(), "real sample TEXT index is empty");
            const auto text_hit = item_types.find_row_by_index("CatItemTypesByName", first_label->at(label));
            check(text_hit && text_hit->id() == first_label->id(),
                  "real sample TEXT index lookup missed its first row");
            auto text_seek = item_types.rows_by_index_from("CatItemTypesByName", first_label->at(label));
            const auto first_seek = text_seek.next();
            check(first_seek && first_seek->id() == first_label->id(),
                  "real sample TEXT index seek did not start at its first row");
        }

        {
            // Writing into an existing ArcGIS/Access table: GDB_SpatialRefs has a single
            // ascending LONG index and no Memo/OLE long-value map of its own, so a row can be
            // inserted and removed against the original index pages. Work on a copy only.
            const auto copy = std::filesystem::temp_directory_path() /
                              "rea-jackcess-real-sample-write.mdb";
            std::filesystem::remove(copy);
            std::filesystem::copy_file(path, copy);

            const std::int32_t inserted_srid = 4242;
            {
                auto writable = rea_jackcess::Database::open(
                    copy, rea_jackcess::DatabaseOpenMode::read_write);
                auto writable_tables = writable.tables();
                auto table = std::find_if(writable_tables.begin(),
                                          writable_tables.end(),
                                          [](const auto& candidate) {
                                              return candidate.name() == "GDB_SpatialRefs";
                                          });
                check(table != writable_tables.end(),
                      "writable real sample did not expose GDB_SpatialRefs");
                check(table->row_count_header() == 3,
                      "real sample GDB_SpatialRefs changed its row count");

                std::vector<rea_jackcess::Value> row;
                for (const auto& column : table->columns()) {
                    if (column.name == "SRID") {
                        row.push_back(inserted_srid);
                    } else if (column.name == "SRTEXT") {
                        row.push_back(rea_jackcess::MemoValue{std::string("GEOGCS[\"test\"]"), {}});
                    } else if (column.type == rea_jackcess::DataType::float64) {
                        row.push_back(1.0);
                    } else if (column.type == rea_jackcess::DataType::int32) {
                        row.push_back(std::int32_t{0});
                    } else {
                        row.push_back(std::monostate{});
                    }
                }
                const auto row_id = table->add_row(row);
                check(table->row_count_header() == 4,
                      "inserting into a real Access table did not raise the row count");

                const auto srid = column_index(*table, "SRID");
                std::vector<std::int32_t> keys;
                auto cursor = table->rows_by_index("SRID_Index");
                while (const auto current = cursor.next()) {
                    keys.push_back(std::get<std::int32_t>(current->at(srid)));
                }
                check(keys == std::vector<std::int32_t>({1, 2, 3, inserted_srid}),
                      "the real table index did not include the inserted row in order");
                const auto found = table->find_row_by_index("SRID_Index", inserted_srid);
                check(found && std::get<std::int32_t>(found->at(srid)) == inserted_srid,
                      "key lookup failed on a row inserted into the real table");

                table->delete_row(row_id);
                check(table->row_count_header() == 3,
                      "deleting from a real Access table did not lower the row count");
            }

            {
                // Scoped so the file handle is closed before the copy is deleted; Windows refuses to
                // remove a file that is still open.
                const auto reopened = rea_jackcess::Database::open(copy);
                for (const auto& current : reopened.tables()) {
                    if (current.name() != "GDB_SpatialRefs") {
                        continue;
                    }
                    check(current.row_count_header() == 3,
                          "reopened real table copy has an unexpected row count");
                    const auto srid = column_index(current, "SRID");
                    std::vector<std::int32_t> keys;
                    auto cursor = current.rows_by_index("SRID_Index");
                    while (const auto row = cursor.next()) {
                        keys.push_back(std::get<std::int32_t>(row->at(srid)));
                    }
                    check(keys == std::vector<std::int32_t>({1, 2, 3}),
                          "the real table index did not return to its original keys");
                }
            }
            std::filesystem::remove(copy);
        }

        // All writes above ran on a copy; the source sample must still hash to the pinned value.
        check(rea_jackcess::detail::sha256_file_hex(path) == pinned_sample_sha256,
              "the real sample changed during the read and copy-write checks");
        std::cout << "real sample reads, index seeks and table writes passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "real sample BOOLEAN reads failed: " << error.what() << '\n';
        return 1;
    }
}
