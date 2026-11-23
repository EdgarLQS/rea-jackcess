#include "api/database_internal.hpp"
#include "catalog/row_reader.hpp"
#include "rea_jackcess/table.hpp"
#include "value/attachment_value.hpp"
#include "value/jet4_value.hpp"

#include <algorithm>
#include <cstdint>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace rea_jackcess {
namespace {

std::uint8_t ascii_lower(std::uint8_t character) {
    return character >= 'A' && character <= 'Z'
               ? static_cast<std::uint8_t>(character + ('a' - 'A'))
               : character;
}

bool column_names_equal(std::string_view left, std::string_view right) {
    return left.size() == right.size() &&
           std::equal(left.begin(), left.end(), right.begin(), [](char lhs, char rhs) {
               return ascii_lower(static_cast<std::uint8_t>(lhs)) ==
                      ascii_lower(static_cast<std::uint8_t>(rhs));
           });
}

const detail::TableState& checked_state(const std::shared_ptr<detail::TableState>& state) {
    if (!state) {
        throw std::logic_error("Table has no state; it may have been moved from");
    }
    return *state;
}

}  // namespace

std::vector<Row> Table::raw_complex_values(std::string_view complex_column_name, RowId parent_row_id) const {
    const auto& state = checked_state(state_);
    auto& database = *state.database;
    std::lock_guard<std::recursive_mutex> lock(database.write_mutex);

    const auto& parent_table = database.schema.tables.at(state.schema_index);
    const auto& parent_definition = parent_table.definition;
    const auto parent_column = std::find_if(parent_definition.columns.begin(), parent_definition.columns.end(),
                                            [&](const auto& candidate) {
        return column_names_equal(candidate.name, complex_column_name);
    });
    if (parent_column == parent_definition.columns.end()) {
        throw std::invalid_argument("Table has no column named '" + std::string(complex_column_name) + "'");
    }
    if (parent_column->type != 0x12) {
        throw std::invalid_argument("Column '" + std::string(complex_column_name) + "' is not complex");
    }

    const auto& parent_column_info = database.columns.at(state.schema_index).at(parent_column->column_number);
    if (!parent_column_info.complex_column_reference) {
        throw DatabaseError(DatabaseErrorCode::corrupted_file,
                            "Complex column has no resolved supporting-table association",
                            database.file.path(), parent_table.definition_page_number,
                            {parent_table.name, parent_row_id.row_number, parent_column->column_number,
                             parent_column->name});
    }
    const auto& reference = *parent_column_info.complex_column_reference;
    if (!reference.foreign_key_column_name) {
        throw DatabaseError(DatabaseErrorCode::corrupted_file,
                            "Complex column has no resolved foreign-key column",
                            database.file.path(), reference.flat_table_page_number,
                            {reference.flat_table_name, std::nullopt, std::nullopt, std::nullopt});
    }

    detail::TableRowReader parent_reader(database.file, parent_table.name, parent_definition);
    const auto parent_row = parent_reader.read_at(parent_row_id.page_number, parent_row_id.row_number);
    const auto foreign_key = detail::decode_jet4_complex_value_foreign_key(
        database.file, parent_table.name, parent_row, *parent_column);
    if (!foreign_key) {
        return {};
    }

    const auto flat_table = std::find_if(database.schema.tables.begin(), database.schema.tables.end(),
                                         [&](const auto& candidate) {
        return candidate.definition_page_number == reference.flat_table_page_number;
    });
    if (flat_table == database.schema.tables.end()) {
        throw DatabaseError(DatabaseErrorCode::corrupted_file,
                            "Complex column references an unknown flat table",
                            database.file.path(), reference.flat_table_page_number,
                            {parent_table.name, parent_row_id.row_number, parent_column->column_number,
                             parent_column->name});
    }
    const auto flat_table_index = static_cast<std::size_t>(flat_table - database.schema.tables.begin());
    const auto& flat_definition = flat_table->definition;
    const auto foreign_key_column = std::find_if(flat_definition.columns.begin(), flat_definition.columns.end(),
                                                 [&](const auto& candidate) {
        return column_names_equal(candidate.name, *reference.foreign_key_column_name);
    });
    if (foreign_key_column == flat_definition.columns.end() || foreign_key_column->type != 0x04) {
        throw DatabaseError(DatabaseErrorCode::corrupted_file,
                            "Complex flat table has no LONG foreign-key column",
                            database.file.path(), reference.flat_table_page_number,
                            {flat_table->name, std::nullopt, std::nullopt, reference.foreign_key_column_name});
    }

    auto flat_state = std::make_shared<detail::TableState>();
    flat_state->database = state.database;
    flat_state->schema_index = flat_table_index;
    flat_state->error_handler = state.error_handler;
    Table flat_table_handle(std::move(flat_state));

    const auto& indexes = database.indexes.at(flat_table_index);
    const auto foreign_key_index = std::find_if(indexes.begin(), indexes.end(), [&](const auto& index) {
        return !index.columns.empty() && column_names_equal(index.columns.front().name,
                                                           foreign_key_column->name);
    });
    std::vector<Row> values;
    if (foreign_key_index != indexes.end()) {
        auto cursor = flat_table_handle.rows_by_index_prefix(foreign_key_index->name, {Value{*foreign_key}});
        while (auto row = cursor.next()) {
            values.push_back(std::move(*row));
        }
        return values;
    }

    const auto encoded_rows = detail::read_table_rows(database.file, flat_table->name, flat_definition);
    for (const auto& encoded_row : encoded_rows) {
        const auto key_value = detail::decode_jet4_value(database.file, flat_table->name,
                                                         encoded_row, *foreign_key_column);
        const auto* key = std::get_if<std::int32_t>(&key_value);
        if (key == nullptr || *key != *foreign_key) {
            continue;
        }
        std::vector<Value> row_values;
        row_values.reserve(flat_definition.columns.size());
        for (const auto& column : flat_definition.columns) {
            row_values.push_back(detail::decode_jet4_value(database.file, flat_table->name,
                                                           encoded_row, column));
        }
        values.emplace_back(Row({encoded_row.page_number, encoded_row.row_number}, std::move(row_values)));
    }
    return values;
}

std::vector<ComplexValueInfo> Table::complex_values(std::string_view complex_column_name,
                                                    RowId parent_row_id) const {
    const auto& state = checked_state(state_);
    auto& database = *state.database;
    std::lock_guard<std::recursive_mutex> lock(database.write_mutex);

    const auto& parent_table = database.schema.tables.at(state.schema_index);
    const auto& parent_definition = parent_table.definition;
    const auto parent_column = std::find_if(parent_definition.columns.begin(), parent_definition.columns.end(),
                                            [&](const auto& candidate) {
        return column_names_equal(candidate.name, complex_column_name);
    });
    if (parent_column == parent_definition.columns.end()) {
        throw std::invalid_argument("Table has no column named '" + std::string(complex_column_name) + "'");
    }
    if (parent_column->type != 0x12) {
        throw std::invalid_argument("Column '" + std::string(complex_column_name) + "' is not complex");
    }

    const auto& public_parent_column = database.columns.at(state.schema_index).at(parent_column->column_number);
    if (!public_parent_column.complex_column_reference) {
        throw DatabaseError(DatabaseErrorCode::corrupted_file,
                            "Complex column has no resolved supporting-table association",
                            database.file.path(), parent_table.definition_page_number,
                            {parent_table.name, parent_row_id.row_number, parent_column->column_number,
                             parent_column->name});
    }
    const auto& reference = *public_parent_column.complex_column_reference;

    const auto flat_table = std::find_if(database.schema.tables.begin(), database.schema.tables.end(),
                                         [&](const auto& candidate) {
        return candidate.definition_page_number == reference.flat_table_page_number;
    });
    if (flat_table == database.schema.tables.end()) {
        throw DatabaseError(DatabaseErrorCode::corrupted_file,
                            "Complex column references an unknown flat table",
                            database.file.path(), reference.flat_table_page_number,
                            {parent_table.name, parent_row_id.row_number, parent_column->column_number,
                             parent_column->name});
    }
    const auto flat_index = static_cast<std::size_t>(flat_table - database.schema.tables.begin());
    const auto& flat_columns = database.columns.at(flat_index);
    const auto find_flat_column = [&](std::string_view name) -> const ColumnInfo* {
        const auto column = std::find_if(flat_columns.begin(), flat_columns.end(), [&](const auto& candidate) {
            return column_names_equal(candidate.name, name);
        });
        return column == flat_columns.end() ? nullptr : &*column;
    };
    const auto row_value = [&](const Row& row, const ColumnInfo* column) -> const Value& {
        if (column == nullptr) {
            throw std::invalid_argument("Complex value column is not present in the flat table");
        }
        return row.at(static_cast<std::size_t>(column - flat_columns.data()));
    };
    const auto* primary_key_column = reference.primary_key_column_name
                                         ? find_flat_column(*reference.primary_key_column_name)
                                         : nullptr;
    const auto* foreign_key_column = reference.foreign_key_column_name
                                         ? find_flat_column(*reference.foreign_key_column_name)
                                         : nullptr;
    if (primary_key_column == nullptr || foreign_key_column == nullptr) {
        throw DatabaseError(DatabaseErrorCode::corrupted_file,
                            "Complex flat table is missing its resolved key columns",
                            database.file.path(), reference.flat_table_page_number,
                            {flat_table->name, std::nullopt, std::nullopt, std::nullopt});
    }

    const auto raw_values = raw_complex_values(complex_column_name, parent_row_id);
    if (raw_values.empty()) {
        return {};
    }

    const auto type_table = std::find_if(database.schema.tables.begin(), database.schema.tables.end(),
                                         [&](const auto& candidate) {
        return candidate.definition_page_number == reference.complex_type_table_page_number;
    });
    const std::size_t type_index = type_table == database.schema.tables.end()
                                       ? database.schema.tables.size()
                                       : static_cast<std::size_t>(type_table - database.schema.tables.begin());
    std::vector<const ColumnInfo*> type_value_columns;
    if (type_table != database.schema.tables.end()) {
        const auto& type_columns = database.columns.at(type_index);
        for (const auto& flat_column : flat_columns) {
            const auto matching_type_column = std::find_if(type_columns.begin(), type_columns.end(),
                                                           [&](const auto& candidate) {
                return column_names_equal(candidate.name, flat_column.name);
            });
            if (matching_type_column != type_columns.end()) {
                type_value_columns.push_back(&flat_column);
            }
        }
    }

    const auto require_type_column = [&](const ColumnInfo* column, std::string_view role) {
        if (column == nullptr) {
            throw DatabaseError(DatabaseErrorCode::corrupted_file,
                                "Complex type table is missing its " + std::string(role) + " column",
                                database.file.path(), reference.complex_type_table_page_number,
                                {flat_table->name, std::nullopt, std::nullopt, std::nullopt});
        }
    };
    const auto find_type_column = [&](DataType type) -> const ColumnInfo* {
        const auto column = std::find_if(type_value_columns.begin(), type_value_columns.end(),
                                         [&](const auto* candidate) {
            return candidate->type == type;
        });
        return column == type_value_columns.end() ? nullptr : *column;
    };
    const auto optional_text = [&](const Row& row, const ColumnInfo* column) -> std::optional<std::string> {
        if (column == nullptr) {
            return std::nullopt;
        }
        const auto& value = row_value(row, column);
        if (std::holds_alternative<std::monostate>(value)) {
            return std::nullopt;
        }
        if (const auto* text = std::get_if<std::string>(&value)) {
            return *text;
        }
        if (const auto* memo = std::get_if<MemoValue>(&value)) {
            return memo->text;
        }
        throw DatabaseError(DatabaseErrorCode::corrupted_file,
                            "Complex text field has an unexpected decoded value type",
                            database.file.path(), reference.flat_table_page_number,
                            {flat_table->name, row.id().row_number,
                             static_cast<std::uint16_t>(column - flat_columns.data()), column->name});
    };

    const auto complex_type = reference.type.value_or(ComplexColumnType::unsupported);
    const ColumnInfo* multi_value_column = nullptr;
    const ColumnInfo* version_value_column = nullptr;
    const ColumnInfo* version_modified_column = nullptr;
    const ColumnInfo* attachment_url_column = nullptr;
    const ColumnInfo* attachment_name_column = nullptr;
    const ColumnInfo* attachment_type_column = nullptr;
    const ColumnInfo* attachment_data_column = nullptr;
    const ColumnInfo* attachment_timestamp_column = nullptr;
    const ColumnInfo* attachment_flags_column = nullptr;

    if (complex_type == ComplexColumnType::multi_value) {
        if (type_value_columns.size() != 1) {
            throw DatabaseError(DatabaseErrorCode::corrupted_file,
                                "Multi-value type table does not have exactly one value column",
                                database.file.path(), reference.complex_type_table_page_number,
                                {flat_table->name, std::nullopt, std::nullopt, std::nullopt});
        }
        multi_value_column = type_value_columns.front();
    } else if (complex_type == ComplexColumnType::version_history) {
        version_value_column = find_type_column(DataType::memo);
        version_modified_column = find_type_column(DataType::short_date_time);
        require_type_column(version_value_column, "version text");
        require_type_column(version_modified_column, "version timestamp");
    } else if (complex_type == ComplexColumnType::attachment) {
        for (const auto* column : type_value_columns) {
            switch (column->type) {
                case DataType::text:
                    if (column_names_equal(column->name, "FileName")) {
                        attachment_name_column = column;
                    } else if (column_names_equal(column->name, "FileType")) {
                        attachment_type_column = column;
                    } else if (attachment_name_column == nullptr) {
                        attachment_name_column = column;
                    } else if (attachment_type_column == nullptr) {
                        attachment_type_column = column;
                    }
                    break;
                case DataType::memo:
                    attachment_url_column = column;
                    break;
                case DataType::ole:
                    attachment_data_column = column;
                    break;
                case DataType::short_date_time:
                    attachment_timestamp_column = column;
                    break;
                case DataType::int32:
                    attachment_flags_column = column;
                    break;
                default:
                    break;
            }
        }
        require_type_column(attachment_url_column, "attachment URL");
        require_type_column(attachment_name_column, "attachment file name");
        require_type_column(attachment_type_column, "attachment file type");
        require_type_column(attachment_data_column, "attachment data");
        require_type_column(attachment_timestamp_column, "attachment timestamp");
        require_type_column(attachment_flags_column, "attachment flags");
    }

    std::vector<ComplexValueInfo> result;
    result.reserve(raw_values.size());
    for (const auto& raw_value : raw_values) {
        const auto* id = std::get_if<std::int32_t>(&row_value(raw_value, primary_key_column));
        const auto* foreign_key = std::get_if<std::int32_t>(&row_value(raw_value, foreign_key_column));
        if (id == nullptr || foreign_key == nullptr) {
            throw DatabaseError(DatabaseErrorCode::corrupted_file,
                                "Complex flat-table key did not decode as LONG",
                                database.file.path(), reference.flat_table_page_number,
                                {flat_table->name, raw_value.id().row_number, std::nullopt, std::nullopt});
        }

        ComplexValuePayload payload = UnsupportedComplexValue{};
        if (complex_type == ComplexColumnType::multi_value) {
            payload = MultiValueItem{row_value(raw_value, multi_value_column)};
        } else if (complex_type == ComplexColumnType::version_history) {
            const auto text = optional_text(raw_value, version_value_column);
            payload = VersionHistoryValue{text, row_value(raw_value, version_modified_column)};
        } else if (complex_type == ComplexColumnType::attachment) {
            AttachmentValue attachment;
            attachment.file_url = optional_text(raw_value, attachment_url_column);
            attachment.file_name = optional_text(raw_value, attachment_name_column);
            attachment.file_type = optional_text(raw_value, attachment_type_column);
            attachment.file_timestamp = row_value(raw_value, attachment_timestamp_column);
            if (const auto* flags = std::get_if<std::int32_t>(
                    &row_value(raw_value, attachment_flags_column))) {
                attachment.file_flags = *flags;
            } else if (!std::holds_alternative<std::monostate>(
                           row_value(raw_value, attachment_flags_column))) {
                throw DatabaseError(DatabaseErrorCode::corrupted_file,
                                    "Attachment flags did not decode as LONG",
                                    database.file.path(), reference.flat_table_page_number,
                                    {flat_table->name, raw_value.id().row_number,
                                     static_cast<std::uint16_t>(attachment_flags_column - flat_columns.data()),
                                     attachment_flags_column->name});
            }
            const auto& data_value = row_value(raw_value, attachment_data_column);
            if (const auto* data = std::get_if<BlobValue>(&data_value)) {
                attachment.encoded_file_data = data->bytes;
                try {
                    attachment.file_data = detail::decode_attachment_file_data(data->bytes);
                } catch (const std::exception& error) {
                    throw DatabaseError(DatabaseErrorCode::corrupted_file,
                                        "Could not decode attachment file data: " + std::string(error.what()),
                                        database.file.path(), reference.flat_table_page_number,
                                        {flat_table->name, raw_value.id().row_number,
                                         static_cast<std::uint16_t>(attachment_data_column - flat_columns.data()),
                                         attachment_data_column->name});
                }
            } else if (!std::holds_alternative<std::monostate>(data_value)) {
                throw DatabaseError(DatabaseErrorCode::corrupted_file,
                                    "Attachment data did not decode as OLE bytes",
                                    database.file.path(), reference.flat_table_page_number,
                                    {flat_table->name, raw_value.id().row_number,
                                     static_cast<std::uint16_t>(attachment_data_column - flat_columns.data()),
                                     attachment_data_column->name});
            }
            payload = std::move(attachment);
        }
        result.push_back(ComplexValueInfo{*id, *foreign_key, raw_value, std::move(payload)});
    }

    if (complex_type == ComplexColumnType::version_history) {
        const auto modified_serial = [](const ComplexValueInfo& value) -> std::optional<double> {
            const auto& version = std::get<VersionHistoryValue>(value.value);
            if (const auto* date = std::get_if<DateTimeValue>(&version.modified_at)) {
                return detail::jet4_date_serial(date->iso8601);
            }
            if (const auto* date = std::get_if<DateOutOfRangeValue>(&version.modified_at)) {
                return date->serial;
            }
            return std::nullopt;
        };
        std::stable_sort(result.begin(), result.end(), [&](const auto& left, const auto& right) {
            const auto left_serial = modified_serial(left);
            const auto right_serial = modified_serial(right);
            if (left_serial && right_serial && *left_serial != *right_serial) {
                return *left_serial > *right_serial;
            }
            if (left_serial.has_value() != right_serial.has_value()) {
                return left_serial.has_value();
            }
            if (left.id != right.id) {
                return left.id > right.id;
            }
            return left.complex_value_foreign_key > right.complex_value_foreign_key;
        });
    }
    return result;
}

}  // namespace rea_jackcess
