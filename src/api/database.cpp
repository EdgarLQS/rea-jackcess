#include "database_internal.hpp"

#include "catalog/row_reader.hpp"
#include "index/index_cursor.hpp"
#include "value/jet4_value.hpp"
#include "write/jet4_database_creator.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace rea_jackcess {
namespace {

DataType public_data_type(std::uint8_t type_code) {
    switch (type_code) {
        case 0x01:
            return DataType::boolean;
        case 0x02:
            return DataType::byte;
        case 0x03:
            return DataType::int16;
        case 0x04:
            return DataType::int32;
        case 0x05:
            return DataType::money;
        case 0x06:
            return DataType::float32;
        case 0x07:
            return DataType::float64;
        case 0x08:
            return DataType::short_date_time;
        case 0x09:
            return DataType::binary;
        case 0x0a:
            return DataType::text;
        case 0x0b:
            return DataType::ole;
        case 0x0c:
            return DataType::memo;
        case 0x0f:
            return DataType::guid;
        case 0x10:
            return DataType::numeric;
        case 0x11:
            return DataType::big_binary;
        case 0x12:
            return DataType::complex;
        case 0x13:
            return DataType::int64;
        case 0x14:
            return DataType::extended_date_time;
        default:
            return DataType::unknown;
    }
}

std::vector<IndexInfo> public_indexes(const detail::TableDefinition& definition) {
    constexpr std::uint8_t unique_index_flag = 0x01;
    constexpr std::uint8_t ignore_nulls_index_flag = 0x02;
    constexpr std::uint8_t required_index_flag = 0x08;
    constexpr std::uint8_t primary_key_index_type = 0x01;
    constexpr std::uint8_t foreign_key_index_type = 0x02;
    constexpr std::uint8_t cascade_flag = 0x01;
    constexpr std::uint8_t cascade_null_flag = 0x02;

    std::vector<bool> physical_is_primary(definition.physical_indexes.size(), false);
    for (const auto& index : definition.indexes) {
        if (index.type == primary_key_index_type) {
            physical_is_primary[index.index_data_number] = true;
        }
    }

    std::vector<IndexInfo> indexes;
    indexes.reserve(definition.indexes.size());
    for (const auto& index : definition.indexes) {
        const auto& physical = definition.physical_indexes[index.index_data_number];
        IndexInfo result{
            index.name,
            index.index_number,
            index.index_data_number,
            {},
            physical.root_page_number,
            physical.unique_entry_count,
            physical.flags,
            physical_is_primary[index.index_data_number] || (physical.flags & unique_index_flag) != 0,
            (physical.flags & ignore_nulls_index_flag) != 0,
            (physical.flags & required_index_flag) != 0,
            index.type == primary_key_index_type,
            index.type == foreign_key_index_type,
            std::nullopt,
            std::nullopt,
            std::nullopt,
            (index.cascade_updates_flags & cascade_flag) != 0,
            (index.cascade_deletes_flags & cascade_flag) != 0,
            (index.cascade_deletes_flags & cascade_null_flag) != 0,
        };
        result.columns.reserve(physical.columns.size());
        for (const auto& indexed_column : physical.columns) {
            const auto column = std::find_if(definition.columns.begin(), definition.columns.end(), [&](const auto& candidate) {
                return candidate.column_number == indexed_column.column_number;
            });
            if (column == definition.columns.end()) {
                throw std::logic_error("Index references a column absent from its table definition");
            }
            result.columns.push_back({column->name, indexed_column.column_number, indexed_column.ascending});
        }
        if (result.foreign_key && index.referenced_index_number >= 0) {
            result.referenced_index_type = index.referenced_index_type;
            result.referenced_index_number = static_cast<std::uint32_t>(index.referenced_index_number);
            result.referenced_table_page_number = index.referenced_table_page_number;
        }
        indexes.push_back(std::move(result));
    }
    return indexes;
}

const detail::DatabaseState& checked_state(const std::shared_ptr<detail::DatabaseState>& state) {
    if (!state) {
        throw std::logic_error("Database has no state; it may have been moved from");
    }
    return *state;
}

const detail::TableState& checked_state(const std::shared_ptr<detail::TableState>& state) {
    if (!state) {
        throw std::logic_error("Table has no state; it may have been moved from");
    }
    return *state;
}

}

namespace detail {

DatabaseState::DatabaseState(PageFile source_file, TableSchemaCatalog source_schema)
    : file(std::move(source_file)), schema(std::move(source_schema)) {
    columns.reserve(schema.tables.size());
    indexes.reserve(schema.tables.size());
    for (const auto& table : schema.tables) {
        std::vector<ColumnInfo> table_columns;
        table_columns.reserve(table.definition.columns.size());
        for (const auto& column : table.definition.columns) {
            table_columns.push_back({column.name,
                                     public_data_type(column.type),
                                     column.type,
                                     column.length,
                                     column.precision,
                                     column.scale,
                                     (column.flags & 0x01) != 0,
                                     column.nullable});
        }
        columns.push_back(std::move(table_columns));
        indexes.push_back(public_indexes(table.definition));
    }
    diagnostics.reserve(schema.errors.size());
    for (const auto& error : schema.errors) {
        std::optional<std::string> table_name;
        if (!error.table_name.empty()) {
            table_name = error.table_name;
        }
        diagnostics.push_back({std::move(table_name), error.message, error.page_number});
    }
}

}

struct RowCursor::Impl {
    explicit Impl(std::shared_ptr<detail::TableState> table_state,
                  std::optional<std::uint32_t> index_root_page_number = std::nullopt)
        : table(std::move(table_state)),
          reader(table->database->file,
                 table->database->schema.tables[table->schema_index].name,
                 table->database->schema.tables[table->schema_index].definition) {
        if (index_root_page_number) {
            const auto& schema_table = table->database->schema.tables[table->schema_index];
            index_reader = std::make_unique<detail::IndexEntryCursor>(
                table->database->file,
                schema_table.name,
                schema_table.definition_page_number,
                *index_root_page_number);
        }
    }

    std::shared_ptr<detail::TableState> table;
    detail::TableRowReader reader;
    std::unique_ptr<detail::IndexEntryCursor> index_reader;
};

Row::Row(RowId id, std::vector<Value> values) : id_(id), values_(std::move(values)) {}

const RowId& Row::id() const noexcept {
    return id_;
}

std::size_t Row::size() const noexcept {
    return values_.size();
}

const std::vector<Value>& Row::values() const noexcept {
    return values_;
}

const Value& Row::at(std::size_t column_index) const {
    return values_.at(column_index);
}

RowCursor::RowCursor(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

RowCursor::~RowCursor() = default;

RowCursor::RowCursor(RowCursor&& other) noexcept = default;

RowCursor& RowCursor::operator=(RowCursor&& other) noexcept = default;

std::optional<Row> RowCursor::next() {
    if (!impl_) {
        return std::nullopt;
    }
    std::optional<detail::EncodedTableRow> encoded_row;
    if (impl_->index_reader) {
        const auto location = impl_->index_reader->next();
        if (location) {
            encoded_row = impl_->reader.read_at(location->page_number, location->row_number);
        }
    } else {
        encoded_row = impl_->reader.next();
    }
    if (!encoded_row) {
        return std::nullopt;
    }
    const auto& table = impl_->table->database->schema.tables[impl_->table->schema_index];
    std::vector<Value> values;
    values.reserve(table.definition.columns.size());
    for (const auto& column : table.definition.columns) {
        values.push_back(detail::decode_jet4_value(impl_->table->database->file,
                                                   table.name,
                                                   *encoded_row,
                                                   column));
    }
    return Row({encoded_row->page_number, encoded_row->row_number}, std::move(values));
}

Table::Table(std::shared_ptr<detail::TableState> state) : state_(std::move(state)) {}

const std::string& Table::name() const {
    const auto& state = checked_state(state_);
    return state.database->schema.tables[state.schema_index].name;
}

const std::string& Table::object_type() const {
    const auto& state = checked_state(state_);
    return state.database->schema.tables[state.schema_index].object_type;
}

bool Table::is_user_table() const {
    const auto& state = checked_state(state_);
    return state.database->schema.tables[state.schema_index].is_user_table;
}

bool Table::is_linked() const {
    const auto& state = checked_state(state_);
    return state.database->schema.tables[state.schema_index].is_linked;
}

std::uint32_t Table::row_count_header() const {
    const auto& state = checked_state(state_);
    return state.database->schema.tables[state.schema_index].definition.row_count;
}

const std::vector<ColumnInfo>& Table::columns() const {
    const auto& state = checked_state(state_);
    return state.database->columns[state.schema_index];
}

const std::vector<IndexInfo>& Table::indexes() const {
    const auto& table = checked_state(state_);
    return table.database->indexes.at(table.schema_index);
}

RowCursor Table::rows() const {
    checked_state(state_);
    return RowCursor(std::make_unique<RowCursor::Impl>(state_));
}

RowCursor Table::rows_by_index(const std::string& index_name) const {
    const auto& state = checked_state(state_);
    const auto& indexes = state.database->indexes.at(state.schema_index);
    const auto index = std::find_if(indexes.begin(), indexes.end(), [&](const IndexInfo& candidate) {
        return candidate.name == index_name;
    });
    if (index == indexes.end()) {
        throw std::invalid_argument("Table has no index named '" + index_name + "'");
    }
    return RowCursor(std::make_unique<RowCursor::Impl>(state_, index->root_page_number));
}

Database::Database(std::shared_ptr<detail::DatabaseState> state) : state_(std::move(state)) {}

Database Database::open(const std::filesystem::path& path) {
    auto file = detail::PageFile::open(path);
    auto schema = detail::read_table_schema_catalog(file);
    return Database(std::make_shared<detail::DatabaseState>(std::move(file), std::move(schema)));
}

Database Database::create(const std::filesystem::path& path, DatabaseFormat format) {
    auto staged = detail::stage_jet4_database(path, format);
    {
        auto validation_database = Database::open(staged.temporary_path());
        (void)validation_database.tables();
    }
    staged.publish();
    return Database::open(path);
}

const std::filesystem::path& Database::path() const {
    return checked_state(state_).file.path();
}

const FormatInfo& Database::format() const {
    return checked_state(state_).file.format();
}

const std::vector<DatabaseDiagnostic>& Database::diagnostics() const {
    return checked_state(state_).diagnostics;
}

std::vector<Table> Database::tables() const {
    const auto& state = checked_state(state_);
    std::vector<Table> tables;
    tables.reserve(state.schema.tables.size());
    for (std::size_t index = 0; index < state.schema.tables.size(); ++index) {
        auto table_state = std::make_shared<detail::TableState>();
        table_state->database = state_;
        table_state->schema_index = index;
        tables.push_back(Table(std::move(table_state)));
    }
    return tables;
}

}
