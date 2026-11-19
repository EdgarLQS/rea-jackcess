#pragma once

#include "../catalog/row_reader.hpp"
#include "rea_jackcess/value.hpp"

namespace rea_jackcess::detail {

using DecimalValue = rea_jackcess::DecimalValue;
using DateTimeValue = rea_jackcess::DateTimeValue;
using DateOutOfRangeValue = rea_jackcess::DateOutOfRangeValue;
using GuidValue = rea_jackcess::GuidValue;
using MemoValue = rea_jackcess::MemoValue;
using BlobValue = rea_jackcess::BlobValue;
using UnsupportedValue = rea_jackcess::UnsupportedValue;
using Jet4Value = rea_jackcess::Value;

Jet4Value decode_jet4_value(const PageFile& file,
                            const std::string& table_name,
                            const EncodedTableRow& row,
                            const ColumnDefinition& column);

}
