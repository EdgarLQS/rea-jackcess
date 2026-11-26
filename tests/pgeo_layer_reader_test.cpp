#include "rea_jackcess/pgeo/layer_reader.hpp"

#include "util/sha256.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace {

using rea_jackcess::DataType;
using rea_jackcess::Database;
using rea_jackcess::DatabaseError;
using rea_jackcess::DatabaseErrorCode;
using rea_jackcess::DatabaseFormat;
using rea_jackcess::GuidValue;
using rea_jackcess::MemoValue;
using rea_jackcess::Table;
using rea_jackcess::TableBuilder;
using rea_jackcess::Value;
using rea_jackcess::pgeo::Dataset;

constexpr const char* feature_class_guid = "{70737809-852c-4a03-9e22-2cecea5b9bfa}";
constexpr const char* sample_sha256 =
    "0bc0acb9c732de1e17c43c2d0a6019f7b5a62ccf83c5818a1bc42db7dbc460c6";

void check(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

class TemporaryDatabase {
public:
    explicit TemporaryDatabase(std::string suffix = {}) {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
                ("rea-jackcess-pgeo-" + std::to_string(stamp) + suffix + ".mdb");
        std::filesystem::remove(path_);
        std::filesystem::remove(journal_path());
    }

    ~TemporaryDatabase() {
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
        std::filesystem::remove(journal_path(), ignored);
    }

    const std::filesystem::path& path() const noexcept {
        return path_;
    }

private:
    std::filesystem::path journal_path() const {
        return std::filesystem::path(path_.string() + ".reajournal");
    }

    std::filesystem::path path_;
};

template <typename TableCollection>
auto& require_table(TableCollection& tables, const std::string& name) {
    const auto found = std::find_if(tables.begin(), tables.end(), [&](const auto& table) {
        return table.name() == name;
    });
    check(found != tables.end(), "test database is missing table " + name);
    return *found;
}

std::size_t column_index(const Table& table, const std::string& name) {
    const auto& columns = table.columns();
    for (std::size_t index = 0; index < columns.size(); ++index) {
        if (columns[index].name == name) {
            return index;
        }
    }
    throw std::runtime_error("test table is missing column " + name);
}

bool same_value(const Value& left, const Value& right) {
    if (left.index() != right.index()) {
        return false;
    }
    return std::visit(
        [&](const auto& left_value) {
            using Type = std::decay_t<decltype(left_value)>;
            const auto* right_value = std::get_if<Type>(&right);
            if (right_value == nullptr) {
                return false;
            }
            if constexpr (std::is_same_v<Type, rea_jackcess::MemoValue>) {
                return left_value.text == right_value->text &&
                       left_value.raw_bytes == right_value->raw_bytes;
            } else if constexpr (std::is_same_v<Type, rea_jackcess::BlobValue>) {
                return left_value.bytes == right_value->bytes;
            } else if constexpr (std::is_same_v<Type, rea_jackcess::DateTimeValue>) {
                return left_value.iso8601 == right_value->iso8601;
            } else if constexpr (std::is_same_v<Type, rea_jackcess::DateOutOfRangeValue>) {
                return left_value.serial == right_value->serial ||
                       (std::isnan(left_value.serial) && std::isnan(right_value->serial));
            } else if constexpr (std::is_same_v<Type, rea_jackcess::DecimalValue>) {
                return left_value.text == right_value->text;
            } else if constexpr (std::is_same_v<Type, rea_jackcess::GuidValue>) {
                return left_value.text == right_value->text;
            } else if constexpr (std::is_same_v<Type, rea_jackcess::UnsupportedValue>) {
                return left_value.description == right_value->description &&
                       left_value.raw_bytes == right_value->raw_bytes;
            } else if constexpr (std::is_same_v<Type, float> || std::is_same_v<Type, double>) {
                return left_value == *right_value ||
                       (std::isnan(left_value) && std::isnan(*right_value));
            } else {
                return left_value == *right_value;
            }
        },
        left);
}

void create_synthetic_database(const std::filesystem::path& path, bool include_srs) {
    std::vector<TableBuilder> schema;

    TableBuilder item_types("GDB_ItemTypes");
    item_types.add_column("UUID", DataType::guid).add_column("Name", DataType::text);
    schema.push_back(item_types);

    TableBuilder items("GDB_Items");
    items.add_column("Type", DataType::guid)
        .add_column("Name", DataType::text)
        .add_column("PhysicalName", DataType::text)
        .add_column("DatasetInfo1", DataType::text);
    schema.push_back(items);

    TableBuilder geometry("GDB_GeomColumns");
    geometry.add_column("TableName", DataType::text)
        .add_column("FieldName", DataType::text)
        .add_column("ShapeType", DataType::int32)
        .add_column("SRID", DataType::int32)
        .add_column("HasZ", DataType::boolean)
        .add_column("HasM", DataType::boolean);
    schema.push_back(geometry);

    if (include_srs) {
        TableBuilder spatial_refs("GDB_SpatialRefs");
        spatial_refs.add_column("SRID", DataType::int32)
            .add_column("SRTEXT", DataType::memo);
        schema.push_back(spatial_refs);
    }

    TableBuilder features("FeatureData");
    features.add_column("OBJECTID", DataType::int32)
        .add_column("Shape", DataType::ole)
        .add_column("Label", DataType::text);
    schema.push_back(features);

    TableBuilder empty_features("EmptyFeatures");
    empty_features.add_column("OBJECTID", DataType::int32)
        .add_column("Shape", DataType::ole);
    schema.push_back(empty_features);

    {
        const auto database = Database::create(path, DatabaseFormat::v2003, schema);
    }

    auto writable = Database::open(path, rea_jackcess::DatabaseOpenMode::read_write);
    auto tables = writable.tables();
    require_table(tables, "GDB_ItemTypes")
        .add_row({GuidValue{feature_class_guid}, std::string("Feature Class")});
    require_table(tables, "GDB_Items")
        .add_rows({{GuidValue{feature_class_guid},
                    std::string("PointLayer"),
                    std::string("FeatureData"),
                    std::string("Shape")},
                   {GuidValue{feature_class_guid},
                    std::string("EmptyLayer"),
                    std::string("EmptyFeatures"),
                    std::string("Shape")}});
    require_table(tables, "GDB_GeomColumns")
        .add_rows({{std::string("FeatureData"),
                    std::string("Shape"),
                    std::int32_t{1},
                    std::int32_t{4326},
                    false,
                    false},
                   {std::string("EmptyFeatures"),
                    std::string("Shape"),
                    std::int32_t{1},
                    std::monostate{},
                    false,
                    false}});
    if (include_srs) {
        require_table(tables, "GDB_SpatialRefs")
            .add_row({std::int32_t{4326}, MemoValue{"EPSG:4326", {}}});
    }
    require_table(tables, "FeatureData")
        .add_rows({{std::int32_t{10}, std::monostate{}, std::string("null-shape")},
                   {std::int32_t{11}, rea_jackcess::BlobValue{}, std::string("empty-shape")},
                   {std::int32_t{12},
                    rea_jackcess::BlobValue{{0x01, 0x7f, 0xff}},
                    std::string("raw-shape")}});
}

void test_non_pgeo_is_rejected() {
    TemporaryDatabase file("-ordinary");
    TableBuilder table("Ordinary");
    table.add_column("Id", DataType::int32);
    {
        const auto database = Database::create(file.path(), DatabaseFormat::v2003, {table});
    }
    try {
        (void)Dataset::open(file.path());
        throw std::runtime_error("ordinary database was accepted as PGeo");
    } catch (const DatabaseError& error) {
        check(error.code() == DatabaseErrorCode::unsupported_operation,
              "ordinary database returned the wrong PGeo error code");
    }
}

void test_synthetic_feature_read() {
    TemporaryDatabase file("-synthetic");
    create_synthetic_database(file.path(), true);

    auto reader = [&]() {
        auto dataset = Dataset::open(file.path());
        check(dataset.layers().size() == 2, "synthetic PGeo database did not expose both layers");
        const auto& layer = dataset.layers().front();
        check(layer.name == "PointLayer" && layer.table_name == "FeatureData",
              "synthetic PGeo layer metadata did not preserve its catalog names");
        check(layer.object_id_column == "OBJECTID" && layer.shape_column == "Shape" &&
                  layer.shape_type == 1 && !layer.has_z && !layer.has_m,
              "synthetic PGeo geometry metadata was not mapped");
        check(layer.spatial_reference_id == 4326 && layer.spatial_reference &&
                  layer.spatial_reference->id == 4326 &&
                  layer.spatial_reference->definition == std::optional<std::string>("EPSG:4326"),
              "synthetic PGeo spatial reference was not read");
        check(layer.attributes.size() == 1 && layer.attributes.front().name == "Label" &&
                  layer.attributes.front().type == DataType::text,
              "synthetic PGeo attribute schema was not exposed");
        auto empty_reader = dataset.open_layer("EmptyLayer");
        check(!empty_reader.next(), "empty PGeo layer unexpectedly returned a feature");
        return dataset.open_layer("PointLayer");
    }();

    const auto null_shape = reader.next();
    const auto empty_shape = reader.next();
    const auto raw_shape = reader.next();
    check(null_shape && null_shape->object_id == 10 && !null_shape->shape,
          "NULL SHAPE was not preserved");
    check(empty_shape && empty_shape->object_id == 11 && empty_shape->shape &&
              empty_shape->shape->empty(),
          "zero-length SHAPE was not distinguished from NULL");
    check(raw_shape && raw_shape->object_id == 12 &&
              raw_shape->shape == std::optional<std::vector<std::uint8_t>>({0x01, 0x7f, 0xff}),
          "raw SHAPE bytes were not preserved");
    check(std::get<std::string>(null_shape->attributes.front().value) == "null-shape" &&
              std::get<std::string>(empty_shape->attributes.front().value) == "empty-shape" &&
              std::get<std::string>(raw_shape->attributes.front().value) == "raw-shape",
          "typed PGeo attributes were not read");
    check(!reader.next(), "PGeo feature reader returned rows past the end");

    auto no_srs_file = TemporaryDatabase("-no-srs");
    create_synthetic_database(no_srs_file.path(), false);
    const auto no_srs = Dataset::open(no_srs_file.path());
    check(no_srs.layers().front().spatial_reference_id == 4326 &&
              !no_srs.layers().front().spatial_reference,
          "missing GDB_SpatialRefs did not leave the layer SRS unresolved");

    auto missing_srid_file = TemporaryDatabase("-missing-srid");
    std::filesystem::copy_file(file.path(), missing_srid_file.path());
    {
        auto writable = Database::open(missing_srid_file.path(),
                                       rea_jackcess::DatabaseOpenMode::read_write);
        auto tables = writable.tables();
        auto& geometry = require_table(tables, "GDB_GeomColumns");
        auto cursor = geometry.rows();
        const auto row = cursor.next();
        check(row.has_value(), "synthetic geometry catalog is empty");
        auto values = row->values();
        values[3] = std::int32_t{999};
        geometry.update_row(row->id(), values);
    }
    const auto unresolved = Dataset::open(missing_srid_file.path());
    check(unresolved.layers().front().spatial_reference_id == 999 &&
              !unresolved.layers().front().spatial_reference,
          "missing SRID row did not remain explicitly unresolved");

    auto broken_file = TemporaryDatabase("-broken");
    std::filesystem::copy_file(file.path(), broken_file.path());
    {
        auto writable = Database::open(broken_file.path(), rea_jackcess::DatabaseOpenMode::read_write);
        auto tables = writable.tables();
        auto& items = require_table(tables, "GDB_Items");
        auto cursor = items.rows();
        const auto row = cursor.next();
        check(row.has_value(), "synthetic item catalog is empty");
        auto values = row->values();
        values[2] = std::string("MissingTable");
        items.update_row(row->id(), values);
    }
    try {
        (void)Dataset::open(broken_file.path());
        throw std::runtime_error("broken PGeo layer reference was accepted");
    } catch (const DatabaseError& error) {
        check(error.code() == DatabaseErrorCode::corrupted_file,
              "broken PGeo layer reference returned the wrong error code");
    }
}

bool ascii_equal_ignore_case(std::string_view left, std::string_view right) {
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t index = 0; index < left.size(); ++index) {
        const auto fold = [](unsigned char character) {
            return character >= 'A' && character <= 'Z'
                       ? static_cast<unsigned char>(character + ('a' - 'A'))
                       : character;
        };
        if (fold(static_cast<unsigned char>(left[index])) !=
            fold(static_cast<unsigned char>(right[index]))) {
            return false;
        }
    }
    return true;
}

std::string normalized_guid(const Value& value) {
    const auto* guid = std::get_if<GuidValue>(&value);
    if (guid == nullptr) {
        return {};
    }
    std::string result;
    result.reserve(32);
    for (const char character : guid->text) {
        if (character == '{' || character == '}' || character == '-') {
            continue;
        }
        const auto byte = static_cast<unsigned char>(character);
        if (byte >= 'A' && byte <= 'F') {
            result.push_back(static_cast<char>(byte + ('a' - 'A')));
        } else if ((byte >= 'a' && byte <= 'f') || (byte >= '0' && byte <= '9')) {
            result.push_back(static_cast<char>(byte));
        } else {
            return {};
        }
    }
    return result.size() == 32 ? result : std::string{};
}

const std::string& require_text(const Value& value, const std::string& context) {
    const auto* text = std::get_if<std::string>(&value);
    check(text != nullptr, context + " is not text");
    return *text;
}

std::int32_t require_int32(const Value& value, const std::string& context) {
    const auto* integer = std::get_if<std::int32_t>(&value);
    check(integer != nullptr, context + " is not a LONG integer");
    return *integer;
}

std::optional<std::int32_t> optional_int32(const Value& value, const std::string& context) {
    if (std::holds_alternative<std::monostate>(value)) {
        return std::nullopt;
    }
    return require_int32(value, context);
}

bool require_boolean(const Value& value, const std::string& context) {
    const auto* boolean = std::get_if<bool>(&value);
    check(boolean != nullptr, context + " is not BOOLEAN");
    return *boolean;
}

std::optional<std::string> optional_text(const Value& value, const std::string& context) {
    if (std::holds_alternative<std::monostate>(value)) {
        return std::nullopt;
    }
    if (const auto* text = std::get_if<std::string>(&value)) {
        return *text;
    }
    if (const auto* memo = std::get_if<MemoValue>(&value)) {
        return memo->text;
    }
    throw std::runtime_error(context + " is not text or MEMO");
}

bool same_complex_reference(const std::optional<rea_jackcess::ComplexColumnReference>& left,
                            const std::optional<rea_jackcess::ComplexColumnReference>& right) {
    if (left.has_value() != right.has_value()) {
        return false;
    }
    if (!left) {
        return true;
    }
    return left->conceptual_table_page_number == right->conceptual_table_page_number &&
           left->complex_type_table_page_number == right->complex_type_table_page_number &&
           left->complex_type_table_name == right->complex_type_table_name &&
           left->type == right->type &&
           left->flat_table_page_number == right->flat_table_page_number &&
           left->flat_table_name == right->flat_table_name &&
           left->primary_key_column_name == right->primary_key_column_name &&
           left->foreign_key_column_name == right->foreign_key_column_name;
}

bool same_column_definition(const rea_jackcess::ColumnInfo& left,
                            const rea_jackcess::ColumnInfo& right) {
    return left.name == right.name && left.type == right.type &&
           left.raw_type_code == right.raw_type_code && left.size == right.size &&
           left.precision == right.precision && left.scale == right.scale &&
           left.fixed == right.fixed && left.calculated == right.calculated &&
           left.nullable == right.nullable && left.complex_type_id == right.complex_type_id &&
           left.complex_value_foreign_key == right.complex_value_foreign_key &&
           same_complex_reference(left.complex_column_reference,
                                  right.complex_column_reference);
}

struct ExpectedLayer {
    std::string name;
    std::string table_name;
    std::string shape_column;
    std::string object_id_column;
    std::int32_t shape_type;
    std::optional<std::int32_t> spatial_reference_id;
    bool has_z;
    bool has_m;
    bool has_spatial_reference_row;
    std::optional<std::string> spatial_reference_text;
};

void assert_sample_layers_match_core_catalogs(const std::vector<Table>& core_tables,
                                              const Dataset& dataset) {
    const Table& item_types = require_table(core_tables, "GDB_ItemTypes");
    const std::size_t type_name_column = column_index(item_types, "Name");
    const std::size_t type_uuid_column = column_index(item_types, "UUID");
    auto type_cursor = item_types.rows();
    std::optional<std::string> feature_class_type;
    while (const auto row = type_cursor.next()) {
        if (require_text(row->at(type_name_column), "GDB_ItemTypes.Name") != "Feature Class") {
            continue;
        }
        check(!feature_class_type, "sample has multiple Feature Class type identifiers");
        feature_class_type = normalized_guid(row->at(type_uuid_column));
        check(!feature_class_type->empty(), "Feature Class type identifier is not a GUID");
    }
    check(feature_class_type.has_value(), "sample has no Feature Class type identifier");

    const Table& items = require_table(core_tables, "GDB_Items");
    const std::size_t item_type_column = column_index(items, "Type");
    const std::size_t item_name_column = column_index(items, "Name");
    const std::size_t physical_name_column = column_index(items, "PhysicalName");
    const std::size_t dataset_info_column = column_index(items, "DatasetInfo1");
    const Table& geometry = require_table(core_tables, "GDB_GeomColumns");
    const std::size_t geometry_table_column = column_index(geometry, "TableName");
    const std::size_t geometry_field_column = column_index(geometry, "FieldName");
    const std::size_t geometry_shape_type_column = column_index(geometry, "ShapeType");
    const std::size_t geometry_srid_column = column_index(geometry, "SRID");
    const std::size_t geometry_has_z_column = column_index(geometry, "HasZ");
    const std::size_t geometry_has_m_column = column_index(geometry, "HasM");

    const Table& spatial_refs = require_table(core_tables, "GDB_SpatialRefs");
    const std::size_t srid_column = column_index(spatial_refs, "SRID");
    const std::size_t srtext_column = column_index(spatial_refs, "SRTEXT");
    std::map<std::int32_t, std::optional<std::string>> spatial_reference_text;
    auto spatial_refs_cursor = spatial_refs.rows();
    while (const auto row = spatial_refs_cursor.next()) {
        const std::int32_t srid = require_int32(row->at(srid_column), "GDB_SpatialRefs.SRID");
        check(spatial_reference_text.emplace(
                  srid, optional_text(row->at(srtext_column), "GDB_SpatialRefs.SRTEXT"))
                  .second,
              "sample has duplicate GDB_SpatialRefs SRID");
    }

    std::vector<ExpectedLayer> expected_layers;
    auto items_cursor = items.rows();
    while (const auto item = items_cursor.next()) {
        if (normalized_guid(item->at(item_type_column)) != *feature_class_type) {
            continue;
        }
        ExpectedLayer expected;
        expected.name = require_text(item->at(item_name_column), "GDB_Items.Name");
        expected.table_name =
            require_text(item->at(physical_name_column), "GDB_Items.PhysicalName");
        expected.shape_column =
            require_text(item->at(dataset_info_column), "GDB_Items.DatasetInfo1");

        auto geometry_cursor = geometry.rows();
        std::optional<std::vector<Value>> matching_geometry;
        while (const auto row = geometry_cursor.next()) {
            const auto& geometry_table =
                require_text(row->at(geometry_table_column), "GDB_GeomColumns.TableName");
            const auto& geometry_field =
                require_text(row->at(geometry_field_column), "GDB_GeomColumns.FieldName");
            if (ascii_equal_ignore_case(geometry_table, expected.table_name) &&
                ascii_equal_ignore_case(geometry_field, expected.shape_column)) {
                check(!matching_geometry, "sample has duplicate geometry catalog mappings");
                matching_geometry = row->values();
            }
        }
        check(matching_geometry.has_value(),
              "sample Feature Class has no matching GDB_GeomColumns row");
        expected.shape_type =
            require_int32(matching_geometry->at(geometry_shape_type_column),
                          "GDB_GeomColumns.ShapeType");
        expected.spatial_reference_id =
            optional_int32(matching_geometry->at(geometry_srid_column),
                           "GDB_GeomColumns.SRID");
        expected.has_z =
            require_boolean(matching_geometry->at(geometry_has_z_column),
                            "GDB_GeomColumns.HasZ");
        expected.has_m =
            require_boolean(matching_geometry->at(geometry_has_m_column),
                            "GDB_GeomColumns.HasM");
        expected.has_spatial_reference_row =
            expected.spatial_reference_id &&
            spatial_reference_text.find(*expected.spatial_reference_id) !=
                spatial_reference_text.end();
        if (expected.has_spatial_reference_row) {
            expected.spatial_reference_text =
                spatial_reference_text.at(*expected.spatial_reference_id);
        }

        const Table& core_table = require_table(core_tables, expected.table_name);
        const auto& columns = core_table.columns();
        const auto object_id = std::find_if(columns.begin(), columns.end(), [](const auto& column) {
            return ascii_equal_ignore_case(column.name, "OBJECTID");
        });
        check(object_id != columns.end(), "sample feature table has no OBJECTID column");
        expected.object_id_column = object_id->name;
        expected_layers.push_back(std::move(expected));
    }

    check(expected_layers.size() == dataset.layers().size(),
          "PGeo adapter layer count differs from the Feature Class catalog");
    for (std::size_t layer_index = 0; layer_index < expected_layers.size(); ++layer_index) {
        const auto& expected = expected_layers[layer_index];
        const auto& layer = dataset.layers()[layer_index];
        check(layer.name == expected.name,
              "PGeo adapter layer order or name differs from GDB_Items");
        check(layer.table_name == expected.table_name &&
                  layer.shape_column == expected.shape_column &&
                  layer.object_id_column == expected.object_id_column &&
                  layer.shape_type == expected.shape_type &&
                  layer.spatial_reference_id == expected.spatial_reference_id &&
                  layer.has_z == expected.has_z && layer.has_m == expected.has_m,
              "PGeo adapter metadata differs for layer " + expected.name);
        check(layer.spatial_reference.has_value() == expected.has_spatial_reference_row,
              "PGeo adapter SRS availability differs for layer " + expected.name);
        if (expected.has_spatial_reference_row) {
            check(layer.spatial_reference->id == *expected.spatial_reference_id &&
                      layer.spatial_reference->definition == expected.spatial_reference_text,
                  "PGeo adapter SRS definition differs for layer " + expected.name);
        }

        const Table& core_table = require_table(core_tables, expected.table_name);
        std::vector<const rea_jackcess::ColumnInfo*> expected_attributes;
        for (const auto& column : core_table.columns()) {
            if (!ascii_equal_ignore_case(column.name, expected.object_id_column) &&
                !ascii_equal_ignore_case(column.name, expected.shape_column)) {
                expected_attributes.push_back(&column);
            }
        }
        check(layer.attributes.size() == expected_attributes.size(),
              "PGeo adapter omitted or added attributes for layer " + expected.name);
        for (std::size_t index = 0; index < expected_attributes.size(); ++index) {
            check(same_column_definition(layer.attributes[index], *expected_attributes[index]),
                  "PGeo adapter attribute definition differs for layer " + expected.name);
        }
    }
}

void test_pinned_sample(const std::filesystem::path& path) {
    check(rea_jackcess::detail::sha256_file_hex(path) == sample_sha256,
          "PGeo sample does not match its pinned SHA-256");
    const auto database = Database::open(path);
    const auto core_tables = database.tables();
    const auto dataset = Dataset::open(path);
    check(dataset.layers().size() == 17,
          "PGeo sample did not expose exactly 17 Feature Classes");
    check(std::none_of(dataset.layers().begin(), dataset.layers().end(), [](const auto& layer) {
              return layer.table_name == "GDB_Items";
          }),
          "PGeo catalog table was incorrectly exposed as a feature layer");
    assert_sample_layers_match_core_catalogs(core_tables, dataset);

    std::size_t feature_count = 0;
    std::size_t nonempty_shapes = 0;
    for (const auto& layer : dataset.layers()) {
        const Table& core_table = require_table(core_tables, layer.table_name);
        const std::size_t object_id_column = column_index(core_table, layer.object_id_column);
        const std::size_t shape_column = column_index(core_table, layer.shape_column);
        std::vector<std::size_t> attribute_columns;
        attribute_columns.reserve(layer.attributes.size());
        for (const auto& attribute : layer.attributes) {
            attribute_columns.push_back(column_index(core_table, attribute.name));
        }

        auto core_cursor = core_table.rows();
        auto reader = dataset.open_layer(layer.name);
        while (const auto feature = reader.next()) {
            const auto core_row = core_cursor.next();
            check(core_row.has_value(), "PGeo adapter returned a row absent from the core table");
            check(std::get<std::int32_t>(core_row->at(object_id_column)) == feature->object_id,
                  "PGeo adapter OBJECTID differs from the core table row");

            const Value& core_shape = core_row->at(shape_column);
            if (std::holds_alternative<std::monostate>(core_shape)) {
                check(!feature->shape, "PGeo adapter changed NULL SHAPE into a value");
            } else {
                const auto* blob = std::get_if<rea_jackcess::BlobValue>(&core_shape);
                check(blob != nullptr && feature->shape && *feature->shape == blob->bytes,
                      "PGeo adapter changed raw SHAPE bytes");
            }

            check(feature->attributes.size() == layer.attributes.size(),
                  "PGeo adapter returned an unexpected number of attributes");
            for (std::size_t index = 0; index < layer.attributes.size(); ++index) {
                check(feature->attributes[index].name == layer.attributes[index].name &&
                          same_value(feature->attributes[index].value,
                                     core_row->at(attribute_columns[index])),
                      "PGeo adapter attribute differs from the core table row");
            }

            ++feature_count;
            check(feature->object_id > 0, "PGeo sample returned an invalid OBJECTID");
            if (feature->shape && !feature->shape->empty()) {
                ++nonempty_shapes;
            }
        }
        check(!core_cursor.next(), "PGeo adapter omitted a core table row");
    }
    check(feature_count == 30 && nonempty_shapes == 30,
          "PGeo sample feature count or raw SHAPE payloads changed");

    const auto point_layer = std::find_if(dataset.layers().begin(),
                                          dataset.layers().end(),
                                          [](const auto& layer) { return layer.name == "二维点"; });
    check(point_layer != dataset.layers().end() && point_layer->shape_type == 1 &&
              point_layer->spatial_reference_id == 3 && point_layer->spatial_reference &&
              point_layer->spatial_reference->definition &&
              point_layer->spatial_reference->definition->find("CGCS2000") != std::string::npos,
          "PGeo sample point layer metadata or SRS text changed");
    auto point_reader = dataset.open_layer("二维点");
    const auto first_point = point_reader.next();
    check(first_point && first_point->object_id == 1 && first_point->shape &&
              first_point->shape->size() == 20 &&
              rea_jackcess::detail::sha256_hex(*first_point->shape) ==
                  "fe859e79559e64a472e1b7fa24a6f617bec55d5c6a28c80244d72d6b9b534c98",
          "PGeo sample raw SHAPE bytes did not match the pinned first point");
    check(rea_jackcess::detail::sha256_file_hex(path) == sample_sha256,
          "PGeo reader changed the source sample");
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string(argv[1]) == "sample") {
            std::cerr << "Usage: rea_jackcess_pgeo_tests sample <sample.mdb>\n";
            return 2;
        }
        test_non_pgeo_is_rejected();
        test_synthetic_feature_read();
        if (argc == 3 && std::string(argv[1]) == "sample") {
            test_pinned_sample(argv[2]);
        } else if (argc != 1) {
            std::cerr << "Usage: rea_jackcess_pgeo_tests [sample <sample.mdb>]\n";
            return 2;
        }
        std::cout << "PGeo layer reader passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "PGeo layer reader failed: " << error.what() << '\n';
        return 1;
    }
}
