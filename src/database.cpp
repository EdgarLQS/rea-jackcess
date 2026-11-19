#include "database_internal.hpp"

#include "catalog/row_reader.hpp"
#include "value/jet4_value.hpp"

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
                                     std::nullopt});
        }
        columns.push_back(std::move(table_columns));
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
    explicit Impl(std::shared_ptr<detail::TableState> table_state)
        : table(std::move(table_state)),
          reader(table->database->file,
                 table->database->schema.tables[table->schema_index].name,
                 table->database->schema.tables[table->schema_index].definition) {}

    std::shared_ptr<detail::TableState> table;
    detail::TableRowReader reader;
};

Row::Row(std::vector<Value> values) : values_(std::move(values)) {}

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
    auto encoded_row = impl_->reader.next();
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
    return Row(std::move(values));
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

RowCursor Table::rows() const {
    checked_state(state_);
    return RowCursor(std::make_unique<RowCursor::Impl>(state_));
}

Database::Database(std::shared_ptr<detail::DatabaseState> state) : state_(std::move(state)) {}

Database Database::open(const std::filesystem::path& path) {
    auto file = detail::PageFile::open(path);
    auto schema = detail::read_table_schema_catalog(file);
    return Database(std::make_shared<detail::DatabaseState>(std::move(file), std::move(schema)));
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
