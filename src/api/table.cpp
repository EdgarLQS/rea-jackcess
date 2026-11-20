#include "rea_jackcess/table.hpp"

#include <utility>

namespace rea_jackcess {

ColumnBuilder::ColumnBuilder(std::string name, DataType type)
    : name_(std::move(name)), type_(type) {}

const std::string& ColumnBuilder::name() const noexcept {
    return name_;
}

DataType ColumnBuilder::type() const noexcept {
    return type_;
}

TableBuilder::TableBuilder(std::string name) : name_(std::move(name)) {}

TableBuilder& TableBuilder::add_column(ColumnBuilder column) {
    columns_.push_back(std::move(column));
    return *this;
}

TableBuilder& TableBuilder::add_column(std::string name, DataType type) {
    return add_column(ColumnBuilder(std::move(name), type));
}

const std::string& TableBuilder::name() const noexcept {
    return name_;
}

const std::vector<ColumnBuilder>& TableBuilder::columns() const noexcept {
    return columns_;
}

}
