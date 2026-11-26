#include "rea_jackcess/pgeo/layer_reader.hpp"

#include <algorithm>
#include <cctype>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>

namespace rea_jackcess::pgeo {
namespace {

bool ascii_equal_ignore_case(std::string_view left, std::string_view right) {
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t index = 0; index < left.size(); ++index) {
        const auto fold = [](unsigned char character) {
            if (character >= 'A' && character <= 'Z') {
                return static_cast<unsigned char>(character + ('a' - 'A'));
            }
            return character;
        };
        if (fold(static_cast<unsigned char>(left[index])) !=
            fold(static_cast<unsigned char>(right[index]))) {
            return false;
        }
    }
    return true;
}

[[noreturn]] void throw_catalog_error(const std::filesystem::path& path,
                                      DatabaseErrorCode code,
                                      std::string message,
                                      std::optional<std::string> table_name = std::nullopt,
                                      std::optional<std::string> column_name = std::nullopt,
                                      std::optional<std::uint64_t> row_number = std::nullopt,
                                      std::optional<std::uint64_t> column_number = std::nullopt) {
    DatabaseErrorContext context;
    context.table_name = std::move(table_name);
    context.column_name = std::move(column_name);
    context.row_number = row_number;
    context.column_number = column_number;
    throw DatabaseError(code, std::move(message), path, std::nullopt, std::move(context));
}

[[noreturn]] void throw_corrupt_catalog(const std::filesystem::path& path,
                                        std::string message,
                                        std::string table_name,
                                        std::string column_name = {},
                                        std::optional<std::uint64_t> row_number = std::nullopt,
                                        std::optional<std::uint64_t> column_number = std::nullopt) {
    throw_catalog_error(path,
                        DatabaseErrorCode::corrupted_file,
                        std::move(message),
                        std::move(table_name),
                        column_name.empty() ? std::nullopt
                                            : std::optional<std::string>(std::move(column_name)),
                        row_number,
                        column_number);
}

std::optional<std::size_t> find_column(const Table& table, std::string_view name) {
    const auto& columns = table.columns();
    for (std::size_t index = 0; index < columns.size(); ++index) {
        if (ascii_equal_ignore_case(columns[index].name, name)) {
            return index;
        }
    }
    return std::nullopt;
}

std::size_t require_column(const Table& table,
                           std::string_view name,
                           const std::filesystem::path& path) {
    const auto index = find_column(table, name);
    if (!index) {
        throw_corrupt_catalog(path,
                              "PGeo catalog is missing required column " + std::string(name),
                              table.name(),
                              std::string(name));
    }
    return *index;
}

const Table* find_table(const std::vector<Table>& tables, std::string_view name) {
    const auto found = std::find_if(tables.begin(), tables.end(), [&](const auto& table) {
        return ascii_equal_ignore_case(table.name(), name);
    });
    return found == tables.end() ? nullptr : &*found;
}

const Value& cell(const Row& row, std::size_t index) {
    return row.at(index);
}

const std::string* string_value(const Value& value) {
    return std::get_if<std::string>(&value);
}

const GuidValue* guid_value(const Value& value) {
    return std::get_if<GuidValue>(&value);
}

std::string normalized_guid(std::string_view value) {
    std::string result;
    result.reserve(32);
    for (const char character : value) {
        if (character == '{' || character == '}' || character == '-') {
            continue;
        }
        const auto byte = static_cast<unsigned char>(character);
        const bool is_hex = (byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'f') ||
                            (byte >= 'A' && byte <= 'F');
        if (!is_hex) {
            return {};
        }
        result.push_back(static_cast<char>(byte >= 'A' && byte <= 'F' ? byte + ('a' - 'A') : byte));
    }
    return result.size() == 32 ? result : std::string{};
}

std::string require_string(const Value& value,
                           const std::filesystem::path& path,
                           const std::string& table_name,
                           const std::string& column_name,
                           std::uint64_t row_number,
                           std::uint64_t column_number) {
    if (const auto* text = string_value(value)) {
        return *text;
    }
    throw_corrupt_catalog(path,
                          "PGeo catalog value is not text",
                          table_name,
                          column_name,
                          row_number,
                          column_number);
}

std::string require_guid_key(const Value& value,
                             const std::filesystem::path& path,
                             const std::string& table_name,
                             const std::string& column_name,
                             std::uint64_t row_number,
                             std::uint64_t column_number) {
    const auto* guid = guid_value(value);
    if (guid != nullptr) {
        const std::string key = normalized_guid(guid->text);
        if (!key.empty()) {
            return key;
        }
    }
    throw_corrupt_catalog(path,
                          "PGeo catalog value is not a valid GUID",
                          table_name,
                          column_name,
                          row_number,
                          column_number);
}

std::int32_t require_int32(const Value& value,
                           const std::filesystem::path& path,
                           const std::string& table_name,
                           const std::string& column_name,
                           std::uint64_t row_number,
                           std::uint64_t column_number) {
    if (const auto* integer = std::get_if<std::int32_t>(&value)) {
        return *integer;
    }
    throw_corrupt_catalog(path,
                          "PGeo catalog value is not a LONG integer",
                          table_name,
                          column_name,
                          row_number,
                          column_number);
}

bool require_boolean(const Value& value,
                     const std::filesystem::path& path,
                     const std::string& table_name,
                     const std::string& column_name,
                     std::uint64_t row_number,
                     std::uint64_t column_number) {
    if (const auto* boolean = std::get_if<bool>(&value)) {
        return *boolean;
    }
    throw_corrupt_catalog(path,
                          "PGeo catalog value is not BOOLEAN",
                          table_name,
                          column_name,
                          row_number,
                          column_number);
}

struct GeometryRecord {
    std::string table_name;
    std::string field_name;
    std::int32_t shape_type;
    std::optional<std::int32_t> spatial_reference_id;
    bool has_z;
    bool has_m;
    std::uint64_t row_number;
};

std::optional<std::string> spatial_reference_text(const Value& value,
                                                  const std::filesystem::path& path,
                                                  std::uint64_t row_number,
                                                  std::uint64_t column_number) {
    if (std::holds_alternative<std::monostate>(value)) {
        return std::nullopt;
    }
    if (const auto* text = std::get_if<std::string>(&value)) {
        return *text;
    }
    if (const auto* memo = std::get_if<MemoValue>(&value)) {
        return memo->text;
    }
    throw_corrupt_catalog(path,
                          "GDB_SpatialRefs.SRTEXT is not text or MEMO",
                          "GDB_SpatialRefs",
                          "SRTEXT",
                          row_number,
                          column_number);
}

std::vector<std::string> projected_columns(const LayerInfo& info) {
    std::vector<std::string> result;
    result.reserve(info.attributes.size() + 2);
    result.push_back(info.object_id_column);
    result.push_back(info.shape_column);
    for (const auto& attribute : info.attributes) {
        result.push_back(attribute.name);
    }
    return result;
}

} // namespace

struct LayerReader::Impl {
    Impl(Table table, LayerInfo layer, std::filesystem::path database_path)
        : info(std::move(layer)),
          path(std::move(database_path)),
          projected_names(projected_columns(info)),
          cursor(table.rows(projected_names)) {}

    LayerInfo info;
    std::filesystem::path path;
    std::vector<std::string> projected_names;
    RowCursor cursor;
};

LayerReader::LayerReader(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
LayerReader::LayerReader(LayerReader&&) noexcept = default;
LayerReader& LayerReader::operator=(LayerReader&&) noexcept = default;
LayerReader::~LayerReader() = default;

std::optional<Feature> LayerReader::next() {
    if (!impl_) {
        return std::nullopt;
    }
    const auto row = impl_->cursor.next();
    if (!row) {
        return std::nullopt;
    }

    const auto bad_row = [&](std::string column, std::uint64_t column_number, std::string message) {
        DatabaseErrorContext context;
        context.table_name = impl_->info.table_name;
        context.column_name = std::move(column);
        context.row_number = row->id().row_number;
        context.column_number = column_number;
        throw DatabaseError(DatabaseErrorCode::corrupted_file,
                            std::move(message),
                            impl_->path,
                            row->id().page_number,
                            std::move(context));
    };

    const auto* object_id = std::get_if<std::int32_t>(&row->at(0));
    if (object_id == nullptr) {
        bad_row(impl_->info.object_id_column, 0, "PGeo OBJECTID value is not a LONG integer");
    }

    Feature feature{*object_id, {}, std::nullopt};
    const Value& shape_value = row->at(1);
    if (std::holds_alternative<std::monostate>(shape_value)) {
        feature.shape = std::nullopt;
    } else if (const auto* blob = std::get_if<BlobValue>(&shape_value)) {
        feature.shape = blob->bytes;
    } else {
        bad_row(impl_->info.shape_column, 1, "PGeo SHAPE value is not a raw BLOB");
    }

    feature.attributes.reserve(impl_->info.attributes.size());
    for (std::size_t index = 0; index < impl_->info.attributes.size(); ++index) {
        feature.attributes.push_back(
            Attribute{impl_->info.attributes[index].name, row->at(index + 2)});
    }
    return feature;
}

Dataset::Dataset(Database database, std::vector<LayerInfo> layers)
    : database_(std::move(database)), layers_(std::move(layers)) {}

Dataset Dataset::open(const std::filesystem::path& path) {
    Database database = Database::open(path);
    const auto tables = database.tables();
    const Table* item_types = find_table(tables, "GDB_ItemTypes");
    const Table* items = find_table(tables, "GDB_Items");
    const Table* geometry_columns = find_table(tables, "GDB_GeomColumns");
    const unsigned int catalog_count = static_cast<unsigned int>(item_types != nullptr) +
                                       static_cast<unsigned int>(items != nullptr) +
                                       static_cast<unsigned int>(geometry_columns != nullptr);
    if (catalog_count == 0) {
        throw_catalog_error(path,
                            DatabaseErrorCode::unsupported_operation,
                            "database does not contain the PGeo layer catalogs");
    }
    if (catalog_count != 3) {
        const std::string missing = item_types == nullptr
                                        ? "GDB_ItemTypes"
                                        : (items == nullptr ? "GDB_Items" : "GDB_GeomColumns");
        throw_corrupt_catalog(path, "PGeo catalog is missing " + missing, missing);
    }

    const std::size_t type_name_column = require_column(*item_types, "Name", path);
    const std::size_t type_uuid_column = require_column(*item_types, "UUID", path);
    std::optional<std::string> feature_class_type;
    {
        auto cursor = item_types->rows();
        std::uint64_t row_number = 0;
        while (const auto row = cursor.next()) {
            ++row_number;
            const std::string name = require_string(cell(*row, type_name_column),
                                                    path,
                                                    item_types->name(),
                                                    "Name",
                                                    row_number,
                                                    type_name_column);
            if (!ascii_equal_ignore_case(name, "Feature Class")) {
                continue;
            }
            const std::string key = require_guid_key(cell(*row, type_uuid_column),
                                                     path,
                                                     item_types->name(),
                                                     "UUID",
                                                     row_number,
                                                     type_uuid_column);
            if (feature_class_type && *feature_class_type != key) {
                throw_corrupt_catalog(path,
                                      "GDB_ItemTypes contains conflicting Feature Class identifiers",
                                      item_types->name(),
                                      "UUID",
                                      row_number,
                                      type_uuid_column);
            }
            if (feature_class_type) {
                throw_corrupt_catalog(path,
                                      "GDB_ItemTypes contains duplicate Feature Class identifiers",
                                      item_types->name(),
                                      "Name",
                                      row_number,
                                      type_name_column);
            }
            feature_class_type = key;
        }
    }
    if (!feature_class_type) {
        throw_corrupt_catalog(path,
                              "GDB_ItemTypes has no Feature Class type",
                              item_types->name(),
                              "Name");
    }

    const std::size_t geom_table_column = require_column(*geometry_columns, "TableName", path);
    const std::size_t geom_field_column = require_column(*geometry_columns, "FieldName", path);
    const std::size_t geom_type_column = require_column(*geometry_columns, "ShapeType", path);
    const std::size_t geom_srid_column = require_column(*geometry_columns, "SRID", path);
    const std::size_t geom_z_column = require_column(*geometry_columns, "HasZ", path);
    const std::size_t geom_m_column = require_column(*geometry_columns, "HasM", path);
    std::vector<GeometryRecord> geometry_records;
    {
        auto cursor = geometry_columns->rows();
        std::uint64_t row_number = 0;
        while (const auto row = cursor.next()) {
            ++row_number;
            GeometryRecord record{
                require_string(cell(*row, geom_table_column),
                               path,
                               geometry_columns->name(),
                               "TableName",
                               row_number,
                               geom_table_column),
                require_string(cell(*row, geom_field_column),
                               path,
                               geometry_columns->name(),
                               "FieldName",
                               row_number,
                               geom_field_column),
                require_int32(cell(*row, geom_type_column),
                              path,
                              geometry_columns->name(),
                              "ShapeType",
                              row_number,
                              geom_type_column),
                std::nullopt,
                require_boolean(cell(*row, geom_z_column),
                                path,
                                geometry_columns->name(),
                                "HasZ",
                                row_number,
                                geom_z_column),
                require_boolean(cell(*row, geom_m_column),
                                path,
                                geometry_columns->name(),
                                "HasM",
                                row_number,
                                geom_m_column),
                row_number};
            if (!std::holds_alternative<std::monostate>(cell(*row, geom_srid_column))) {
                record.spatial_reference_id = require_int32(cell(*row, geom_srid_column),
                                                            path,
                                                            geometry_columns->name(),
                                                            "SRID",
                                                            row_number,
                                                            geom_srid_column);
            }
            if (record.table_name.empty() || record.field_name.empty()) {
                throw_corrupt_catalog(path,
                                      "GDB_GeomColumns contains an empty table or field name",
                                      geometry_columns->name(),
                                      record.table_name.empty() ? "TableName" : "FieldName",
                                      row_number);
            }
            geometry_records.push_back(std::move(record));
        }
    }

    std::map<std::int32_t, SpatialReferenceInfo> spatial_references;
    if (const Table* spatial_refs = find_table(tables, "GDB_SpatialRefs")) {
        const std::size_t srid_column = require_column(*spatial_refs, "SRID", path);
        const std::size_t text_column = require_column(*spatial_refs, "SRTEXT", path);
        auto cursor = spatial_refs->rows();
        std::uint64_t row_number = 0;
        while (const auto row = cursor.next()) {
            ++row_number;
            const std::int32_t id = require_int32(cell(*row, srid_column),
                                                 path,
                                                 spatial_refs->name(),
                                                 "SRID",
                                                 row_number,
                                                 srid_column);
            const auto definition = spatial_reference_text(cell(*row, text_column),
                                                           path,
                                                           row_number,
                                                           text_column);
            if (!spatial_references.emplace(id, SpatialReferenceInfo{id, definition}).second) {
                throw_corrupt_catalog(path,
                                      "GDB_SpatialRefs contains a duplicate SRID",
                                      spatial_refs->name(),
                                      "SRID",
                                      row_number,
                                      srid_column);
            }
        }
    }

    const std::size_t item_type_column = require_column(*items, "Type", path);
    const std::size_t item_name_column = require_column(*items, "Name", path);
    const std::size_t physical_name_column = require_column(*items, "PhysicalName", path);
    const std::size_t geometry_name_column = require_column(*items, "DatasetInfo1", path);
    std::vector<LayerInfo> layers;
    std::set<std::string> layer_names;
    std::set<std::string> physical_names;
    {
        auto cursor = items->rows();
        std::uint64_t row_number = 0;
        while (const auto row = cursor.next()) {
            ++row_number;
            const std::string type_key = require_guid_key(cell(*row, item_type_column),
                                                          path,
                                                          items->name(),
                                                          "Type",
                                                          row_number,
                                                          item_type_column);
            if (type_key != *feature_class_type) {
                continue;
            }

            const std::string name = require_string(cell(*row, item_name_column),
                                                    path,
                                                    items->name(),
                                                    "Name",
                                                    row_number,
                                                    item_name_column);
            const std::string physical_name = require_string(cell(*row, physical_name_column),
                                                             path,
                                                             items->name(),
                                                             "PhysicalName",
                                                             row_number,
                                                             physical_name_column);
            const std::string dataset_geometry_name = require_string(
                cell(*row, geometry_name_column),
                path,
                items->name(),
                "DatasetInfo1",
                row_number,
                geometry_name_column);
            if (name.empty() || physical_name.empty() || dataset_geometry_name.empty()) {
                throw_corrupt_catalog(path,
                                      "GDB_Items Feature Class entry has an empty name or geometry field",
                                      items->name(),
                                      name.empty() ? "Name"
                                                   : (physical_name.empty() ? "PhysicalName"
                                                                            : "DatasetInfo1"),
                                      row_number);
            }

            const auto geometry = std::find_if(
                geometry_records.begin(), geometry_records.end(), [&](const auto& candidate) {
                    return ascii_equal_ignore_case(candidate.table_name, physical_name);
                });
            if (geometry == geometry_records.end()) {
                throw_corrupt_catalog(path,
                                      "Feature Class has no matching GDB_GeomColumns row",
                                      items->name(),
                                      "PhysicalName",
                                      row_number,
                                      physical_name_column);
            }
            if (std::find_if(std::next(geometry), geometry_records.end(), [&](const auto& candidate) {
                    return ascii_equal_ignore_case(candidate.table_name, physical_name);
                }) != geometry_records.end()) {
                throw_corrupt_catalog(path,
                                      "Feature Class has multiple GDB_GeomColumns rows",
                                      geometry_columns->name(),
                                      "TableName",
                                      geometry->row_number,
                                      geom_table_column);
            }
            if (!ascii_equal_ignore_case(geometry->field_name, dataset_geometry_name)) {
                throw_corrupt_catalog(path,
                                      "GDB_Items and GDB_GeomColumns disagree on the geometry field",
                                      geometry_columns->name(),
                                      "FieldName",
                                      geometry->row_number,
                                      geom_field_column);
            }

            const Table* table = find_table(tables, physical_name);
            if (table == nullptr) {
                throw_corrupt_catalog(path,
                                      "PGeo Feature Class references a missing local table",
                                      items->name(),
                                      "PhysicalName",
                                      row_number,
                                      physical_name_column);
            }
            const auto object_id_index = find_column(*table, "OBJECTID");
            const auto shape_index = find_column(*table, geometry->field_name);
            if (!object_id_index) {
                throw_corrupt_catalog(path,
                                      "PGeo Feature Class has no OBJECTID column",
                                      table->name(),
                                      "OBJECTID");
            }
            if (table->columns()[*object_id_index].type != DataType::int32) {
                throw_corrupt_catalog(path,
                                      "PGeo OBJECTID column is not a LONG integer",
                                      table->name(),
                                      table->columns()[*object_id_index].name,
                                      std::nullopt,
                                      *object_id_index);
            }
            if (!shape_index) {
                throw_corrupt_catalog(path,
                                      "PGeo Feature Class has no geometry column",
                                      table->name(),
                                      geometry->field_name);
            }
            const DataType shape_type = table->columns()[*shape_index].type;
            if (shape_type != DataType::ole && shape_type != DataType::binary) {
                throw_corrupt_catalog(path,
                                      "PGeo geometry column is not OLE or BINARY",
                                      table->name(),
                                      table->columns()[*shape_index].name,
                                      std::nullopt,
                                      *shape_index);
            }
            if (!layer_names.insert(name).second ||
                !physical_names.insert(physical_name).second) {
                throw_corrupt_catalog(path,
                                      "PGeo catalog contains a duplicate Feature Class name or table",
                                      items->name(),
                                      "PhysicalName",
                                      row_number,
                                      physical_name_column);
            }

            LayerInfo info{name,
                           table->name(),
                           table->columns()[*object_id_index].name,
                           table->columns()[*shape_index].name,
                           geometry->shape_type,
                           geometry->has_z,
                           geometry->has_m,
                           geometry->spatial_reference_id,
                           std::nullopt,
                           {}};
            for (std::size_t column_index = 0; column_index < table->columns().size(); ++column_index) {
                if (column_index != *object_id_index && column_index != *shape_index) {
                    info.attributes.push_back(table->columns()[column_index]);
                }
            }
            if (info.spatial_reference_id) {
                const auto spatial_reference = spatial_references.find(*info.spatial_reference_id);
                if (spatial_reference != spatial_references.end()) {
                    info.spatial_reference = spatial_reference->second;
                }
            }
            layers.push_back(std::move(info));
        }
    }

    return Dataset(std::move(database), std::move(layers));
}

const std::vector<LayerInfo>& Dataset::layers() const noexcept {
    return layers_;
}

LayerReader Dataset::open_layer(std::string_view name) const {
    const auto info = std::find_if(layers_.begin(), layers_.end(), [&](const auto& candidate) {
        return candidate.name == name;
    });
    if (info == layers_.end()) {
        throw_catalog_error(database_.path(),
                            DatabaseErrorCode::unsupported_operation,
                            "PGeo layer was not found: " + std::string(name),
                            std::string(name));
    }

    const auto tables = database_.tables();
    const Table* table = find_table(tables, info->table_name);
    if (table == nullptr) {
        throw_corrupt_catalog(database_.path(),
                              "PGeo layer table disappeared after catalog discovery",
                              info->table_name);
    }
    return LayerReader(
        std::make_unique<LayerReader::Impl>(*table, *info, database_.path()));
}

} // namespace rea_jackcess::pgeo
