#include "rea_jackcess/table.hpp"

#include <utility>

namespace rea_jackcess {

ColumnBuilder::ColumnBuilder(std::string name, DataType type)
    : name_(std::move(name)), type_(type) {}

ColumnBuilder& ColumnBuilder::with_text_sort_order(std::uint16_t lcid, std::int8_t version) noexcept {
    text_sort_order_ = TextSortOrder{lcid, version};
    return *this;
}

const std::string& ColumnBuilder::name() const noexcept {
    return name_;
}

DataType ColumnBuilder::type() const noexcept {
    return type_;
}

const std::optional<TextSortOrder>& ColumnBuilder::text_sort_order() const noexcept {
    return text_sort_order_;
}

TableBuilder::TableBuilder(std::string name) : name_(std::move(name)) {}

TableBuilder& TableBuilder::add_column(ColumnBuilder column) {
    columns_.push_back(std::move(column));
    return *this;
}

TableBuilder& TableBuilder::add_column(std::string name, DataType type) {
    return add_column(ColumnBuilder(std::move(name), type));
}

IndexBuilder::IndexBuilder(std::string name, std::string column_name, bool unique,
                           bool descending)
    : IndexBuilder(std::move(name), std::vector<IndexColumnBuilder>{{std::move(column_name), !descending}},
                   unique) {}

IndexBuilder::IndexBuilder(std::string name, std::vector<IndexColumnBuilder> columns, bool unique)
    : name_(std::move(name)), columns_(std::move(columns)), unique_(unique) {}

IndexBuilder& IndexBuilder::with_primary_key() noexcept {
    primary_key_ = true;
    return *this;
}

IndexBuilder& IndexBuilder::with_required(bool required) noexcept {
    required_ = required;
    return *this;
}

IndexBuilder& IndexBuilder::with_ignore_nulls(bool ignore_nulls) noexcept {
    ignore_nulls_ = ignore_nulls;
    return *this;
}

const std::string& IndexBuilder::name() const noexcept {
    return name_;
}

const std::vector<IndexColumnBuilder>& IndexBuilder::columns() const noexcept {
    return columns_;
}

const std::string& IndexBuilder::column_name() const noexcept {
    static const std::string empty;
    return columns_.empty() ? empty : columns_.front().name;
}

bool IndexBuilder::unique() const noexcept {
    return unique_ || primary_key_;
}

bool IndexBuilder::descending() const noexcept {
    return !columns_.empty() && !columns_.front().ascending;
}

bool IndexBuilder::primary_key() const noexcept {
    return primary_key_;
}

bool IndexBuilder::required() const noexcept {
    return required_ || primary_key_;
}

bool IndexBuilder::ignore_nulls() const noexcept {
    return ignore_nulls_;
}

TableBuilder& TableBuilder::add_index(IndexBuilder index) {
    indexes_.push_back(std::move(index));
    return *this;
}

const std::string& TableBuilder::name() const noexcept {
    return name_;
}

const std::vector<ColumnBuilder>& TableBuilder::columns() const noexcept {
    return columns_;
}

const std::vector<IndexBuilder>& TableBuilder::indexes() const noexcept {
    return indexes_;
}

}
