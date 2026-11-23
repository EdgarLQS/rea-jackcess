#include "rea_jackcess/database.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    const std::string mode = argc >= 3 ? argv[2] : std::string{};
    const bool single_table = mode == "--single";
    const bool maximum_definition = mode == "--maximum-definition";
    const bool batch_rows = mode == "--batch-rows";
    const bool update_row = mode == "--update-row";
    const bool delete_row = mode == "--delete-row";
    const bool fixed_numeric_rows = mode == "--fixed-numeric-rows";
    const bool money_rows = mode == "--money-rows";
    const bool money_empty = mode == "--money-empty";
    const bool guid_rows = mode == "--guid-rows";
    const bool boolean_rows = mode == "--boolean-rows";
    const bool boolean_empty = mode == "--boolean-empty";
    const bool variable_row_growth = mode == "--variable-rows-growth";
    const bool variable_row_shrink = mode == "--variable-rows";
    const bool variable_rows = variable_row_growth || variable_row_shrink;
    const bool long_value_rows = mode == "--long-value-rows";
    const bool long_value_reuse = mode == "--long-value-reuse";
    const bool index_schema = mode == "--index-schema";
    const bool index_rows = mode == "--index-rows";
    const bool index_maintenance = mode == "--index-maintenance";
    const bool index_money = mode == "--index-money";
    const bool index_split = mode == "--index-split";
    const bool index_merge = mode == "--index-merge";
    const bool index_guid = mode == "--index-guid";
    const bool index_desc = mode == "--index-desc";
    const bool writes_rows = batch_rows || update_row || delete_row || fixed_numeric_rows ||
                             money_rows || guid_rows || boolean_rows || variable_rows || long_value_rows ||
                             long_value_reuse || index_rows || index_maintenance || index_money ||
                             index_split || index_merge || index_guid || index_desc;
    const bool v2003 = argc == 4 && std::string(argv[3]) == "--v2003";
    if (argc > 4 || (argc >= 3 && !single_table && !maximum_definition && !writes_rows &&
                     !money_empty && !boolean_empty && !index_schema)) {
        std::cerr << "Usage: create_tables_for_jackcess_check <database.mdb> "
                     "[--single|--maximum-definition|--batch-rows|--update-row|--delete-row|--fixed-numeric-rows|--money-rows|--money-empty|--guid-rows|--boolean-rows|--boolean-empty|--variable-rows-growth|--variable-rows|--long-value-rows|--long-value-reuse|--index-schema|--index-rows|--index-maintenance|--index-money|--index-split|--index-merge|--index-guid|--index-desc] [--v2003]\n";
        return 2;
    }

    try {
        rea_jackcess::TableBuilder first_table("T");
        first_table.add_column("Id", rea_jackcess::DataType::int32);

        rea_jackcess::TableBuilder second_table("Table_1");
        second_table.add_column("Code", rea_jackcess::DataType::int32)
            .add_column("GroupId", rea_jackcess::DataType::int32);

        rea_jackcess::TableBuilder batch_table("T");
        batch_table.add_column("Id", rea_jackcess::DataType::int32)
            .add_column("GroupId", rea_jackcess::DataType::int32);

        rea_jackcess::TableBuilder fixed_numeric_table("T");
        fixed_numeric_table.add_column("ByteValue", rea_jackcess::DataType::byte)
            .add_column("ShortValue", rea_jackcess::DataType::int16)
            .add_column("LongValue", rea_jackcess::DataType::int32)
            .add_column("SingleValue", rea_jackcess::DataType::float32)
            .add_column("DoubleValue", rea_jackcess::DataType::float64);

        rea_jackcess::TableBuilder money_table("MoneyValues");
        money_table.add_column("Amount", rea_jackcess::DataType::money);

        rea_jackcess::TableBuilder guid_table("Guids");
        guid_table.add_column("Id", rea_jackcess::DataType::int32)
            .add_column("Uuid", rea_jackcess::DataType::guid);

        rea_jackcess::TableBuilder indexed_table("Indexed");
        indexed_table.add_column("Id", rea_jackcess::DataType::int32)
            .add_column("Code", rea_jackcess::DataType::int32)
            .add_column("Name", rea_jackcess::DataType::text)
            .add_index(rea_jackcess::IndexBuilder("IX_Id", "Id"))
            .add_index(rea_jackcess::IndexBuilder("IX_Code", "Code"));

        rea_jackcess::TableBuilder money_index_table("MoneyIndexed");
        money_index_table.add_column("Id", rea_jackcess::DataType::int32)
            .add_column("Amount", rea_jackcess::DataType::money)
            .add_index(rea_jackcess::IndexBuilder("IX_Amount", "Amount"));

        rea_jackcess::TableBuilder guid_index_table("GuidIndexed");
        guid_index_table.add_column("Id", rea_jackcess::DataType::int32)
            .add_column("Uuid", rea_jackcess::DataType::guid)
            .add_index(rea_jackcess::IndexBuilder("IX_Uuid", "Uuid"));

        rea_jackcess::TableBuilder desc_index_table("Desc");
        desc_index_table.add_column("Id", rea_jackcess::DataType::int32)
            .add_index(rea_jackcess::IndexBuilder("IX_Id", "Id", false, true));

        rea_jackcess::TableBuilder merge_table("Merge");
        merge_table.add_column("Id", rea_jackcess::DataType::int32)
            .add_index(rea_jackcess::IndexBuilder("IX_Id", "Id"));

        rea_jackcess::TableBuilder split_table("Split");
        split_table.add_column("Id", rea_jackcess::DataType::int32)
            .add_index(rea_jackcess::IndexBuilder("IX_Id", "Id"));

        rea_jackcess::TableBuilder reuse_table("Reuse");
        reuse_table.add_column("Id", rea_jackcess::DataType::int32)
            .add_column("Memo", rea_jackcess::DataType::memo)
            .add_column("Tail", rea_jackcess::DataType::int32);

        rea_jackcess::TableBuilder boolean_table("Booleans");
        boolean_table.add_column("Id", rea_jackcess::DataType::int32)
            .add_column("Flag", rea_jackcess::DataType::boolean)
            .add_column("Maybe", rea_jackcess::DataType::boolean)
            .add_column("Tail", rea_jackcess::DataType::int32);

        rea_jackcess::TableBuilder variable_table("Variable");
        variable_table.add_column("Id", rea_jackcess::DataType::int32)
            .add_column("Name", rea_jackcess::DataType::text)
            .add_column("Payload", rea_jackcess::DataType::binary)
            .add_column("Note", rea_jackcess::DataType::text);

        rea_jackcess::TableBuilder long_value_table("LongValues");
        long_value_table.add_column("Id", rea_jackcess::DataType::int32)
            .add_column("Memo", rea_jackcess::DataType::memo)
            .add_column("Note", rea_jackcess::DataType::text)
            .add_column("Payload", rea_jackcess::DataType::ole);

        rea_jackcess::TableBuilder* selected_table = &first_table;
        if (index_desc) {
            selected_table = &desc_index_table;
        } else if (index_guid) {
            selected_table = &guid_index_table;
        } else if (index_merge) {
            selected_table = &merge_table;
        } else if (index_split) {
            selected_table = &split_table;
        } else if (index_schema || index_rows || index_maintenance) {
            selected_table = &indexed_table;
        } else if (long_value_reuse) {
            selected_table = &reuse_table;
        } else if (long_value_rows) {
            selected_table = &long_value_table;
        } else if (variable_rows) {
            selected_table = &variable_table;
        } else if (fixed_numeric_rows) {
            selected_table = &fixed_numeric_table;
        } else if (index_money) {
            selected_table = &money_index_table;
        } else if (money_rows || money_empty) {
            selected_table = &money_table;
        } else if (guid_rows) {
            selected_table = &guid_table;
        } else if (boolean_rows || boolean_empty) {
            selected_table = &boolean_table;
        } else if (writes_rows) {
            selected_table = &batch_table;
        }
        std::vector<rea_jackcess::TableBuilder> tables{*selected_table};
        if (!single_table) {
            if (maximum_definition) {
                rea_jackcess::TableBuilder large_table("Large");
                for (char suffix = 'A'; suffix <= 'Z'; ++suffix) {
                    large_table.add_column("C" + std::string(61, 'A') + suffix + '0',
                                           rea_jackcess::DataType::int32);
                }
                tables = {large_table};
            } else if (!writes_rows && !money_empty && !boolean_empty && !index_schema) {
                tables.push_back(second_table);
            }
        }
        {
            const auto format = v2003 ? rea_jackcess::DatabaseFormat::v2003
                                      : rea_jackcess::DatabaseFormat::v2000;
            const auto database = rea_jackcess::Database::create(argv[1], format, tables);
            std::size_t user_table_count = 0;
            for (const auto& table : database.tables()) {
                user_table_count += table.is_user_table() ? 1 : 0;
            }
            std::cout << "Created " << user_table_count << " Jet4 user table(s) in "
                      << std::string(argv[1]) << '\n';
        }

        if (writes_rows) {
            auto database = rea_jackcess::Database::open(
                argv[1], rea_jackcess::DatabaseOpenMode::read_write);
            auto tables_after_open = database.tables();
            std::string expected_table_name = "T";
            if (long_value_reuse) {
                expected_table_name = "Reuse";
            } else if (long_value_rows) {
                expected_table_name = "LongValues";
            } else if (variable_rows) {
                expected_table_name = "Variable";
            } else if (money_rows) {
                expected_table_name = "MoneyValues";
            } else if (guid_rows) {
                expected_table_name = "Guids";
            } else if (index_money) {
                expected_table_name = "MoneyIndexed";
            } else if (index_guid) {
                expected_table_name = "GuidIndexed";
            } else if (index_desc) {
                expected_table_name = "Desc";
            } else if (index_merge) {
                expected_table_name = "Merge";
            } else if (index_split) {
                expected_table_name = "Split";
            } else if (index_rows || index_maintenance) {
                expected_table_name = "Indexed";
            } else if (boolean_rows) {
                expected_table_name = "Booleans";
            }
            const auto table = std::find_if(tables_after_open.begin(), tables_after_open.end(),
                                            [&](const auto& candidate) {
                                                return candidate.name() == expected_table_name;
                                            });
            if (table == tables_after_open.end()) {
                throw std::runtime_error("Read-write reopen did not discover the expected table");
            }

            if (long_value_rows) {
                std::vector<std::uint8_t> initial_payload(5000, 0xa5);
                std::vector<std::uint8_t> updated_payload(8192, 0x6c);
                const std::string initial_memo(12000, 'M');
                const std::string updated_memo(20000, 'U');
                const auto row_ids = table->add_rows({
                    {std::int32_t{1}, rea_jackcess::MemoValue{initial_memo, {}}, std::string("tail"),
                     rea_jackcess::BlobValue{initial_payload}},
                    {std::int32_t{2}, rea_jackcess::MemoValue{"🔥雪", {}}, std::string("N"),
                     rea_jackcess::BlobValue{{}}},
                    {std::int32_t{3}, rea_jackcess::MemoValue{"", {}}, std::string{},
                     rea_jackcess::BlobValue{{}}},
                    {std::int32_t{4}, std::monostate{}, std::monostate{}, std::monostate{}},
                });
                table->update_row(row_ids.front(),
                                  {std::int32_t{1}, rea_jackcess::MemoValue{updated_memo, {}}, std::string("tail"),
                                   rea_jackcess::BlobValue{updated_payload}});
                std::cout << "Wrote and updated multi-page MEMO/OLE values, including Unicode, empty, and NULL rows\n";
            } else if (long_value_reuse) {
                const auto row_id = table->add_row(
                    {std::int32_t{1}, rea_jackcess::MemoValue{std::string(300, 'A'), {}}, std::int32_t{7}});
                for (const char fill : {'B', 'C', 'D', 'E'}) {
                    table->update_row(row_id,
                                      {std::int32_t{1},
                                       rea_jackcess::MemoValue{std::string(300, fill), {}},
                                       std::int32_t{7}});
                }
                std::cout << "Wrote and repeatedly updated a MEMO value that reuses one LVAL page\n";
            } else if (money_rows) {
                const auto row_ids = table->add_rows({
                    {rea_jackcess::DecimalValue{"-2341234.03450"}},
                    {rea_jackcess::DecimalValue{"37"}},
                    {rea_jackcess::DecimalValue{"10000.45"}},
                    {rea_jackcess::DecimalValue{"1e2"}},
                    {rea_jackcess::DecimalValue{"0e1000001"}},
                    {rea_jackcess::DecimalValue{"+922337203685477.5807"}},
                    {rea_jackcess::DecimalValue{"-922337203685477.5808"}},
                });
                table->update_row(row_ids[2], {rea_jackcess::DecimalValue{"-0.0001"}});
                std::cout << "Wrote and updated exact Jet4 MONEY values at both signed boundaries\n";
            } else if (guid_rows) {
                const auto row_ids = table->add_rows({
                    {std::int32_t{1}, rea_jackcess::GuidValue{"{00112233-4455-6677-8899-AABBCCDDEEFF}"}},
                    {std::int32_t{2}, rea_jackcess::GuidValue{"01234567-89ab-cdef-0123-456789abcdef"}},
                });
                table->update_row(row_ids.front(),
                                  {std::int32_t{1},
                                   rea_jackcess::GuidValue{"{fedcba98-7654-3210-fedc-ba9876543210}"}});
                std::cout << "Wrote and updated fixed-width GUID values\n";
            } else if (index_rows) {
                (void)table->add_rows({
                    {std::int32_t{1}, std::int32_t{30}, std::string("a")},
                    {std::int32_t{2}, std::int32_t{10}, std::string("b")},
                    {std::int32_t{3}, std::int32_t{20}, std::string("c")},
                });
                std::cout << "Inserted rows and maintained both user indexes\n";
            } else if (index_desc) {
                (void)table->add_rows({{std::int32_t{3}}, {std::int32_t{1}}, {std::int32_t{2}}});
                std::cout << "Inserted 3, 1, 2 into a descending index\n";
            } else if (index_guid) {
                const auto row_ids = table->add_rows({
                    {std::int32_t{1}, rea_jackcess::GuidValue{"01000000-0000-0000-0000-000000000000"}},
                    {std::int32_t{2}, rea_jackcess::GuidValue{"ffffffff-ffff-ffff-ffff-ffffffffffff"}},
                    {std::int32_t{3}, rea_jackcess::GuidValue{"00000000-0000-0000-0000-000000000001"}},
                    {std::int32_t{4}, rea_jackcess::GuidValue{"00000000-0000-0000-0000-00000000ff00"}},
                });
                table->update_row(row_ids[1],
                                  {std::int32_t{2},
                                   rea_jackcess::GuidValue{"00000000-0000-0000-0000-0000000000ff"}});
                table->delete_row(row_ids[3]);
                std::cout << "Inserted, updated and deleted GUID index rows\n";
            } else if (index_merge) {
                std::vector<std::vector<rea_jackcess::Value>> rows;
                rows.reserve(700);
                for (std::int32_t value = 1; value <= 700; ++value) {
                    rows.push_back({value});
                }
                const auto row_ids = table->add_rows(rows);
                // Draining the lowest leaf must unlink and release that page.
                for (std::size_t index = 0; index < 400; ++index) {
                    table->delete_row(row_ids[index]);
                }
                std::cout << "Inserted 700 rows and drained the first index leaf\n";
            } else if (index_split) {
                std::vector<std::vector<rea_jackcess::Value>> rows;
                rows.reserve(700);
                for (std::int32_t value = 1; value <= 700; ++value) {
                    rows.push_back({value});
                }
                (void)table->add_rows(rows);
                std::cout << "Inserted 700 rows, splitting the index leaf into several pages\n";
            } else if (index_money) {
                const auto row_ids = table->add_rows({
                    {std::int32_t{1}, rea_jackcess::DecimalValue{"100.0000"}},
                    {std::int32_t{2}, rea_jackcess::DecimalValue{"-0.0001"}},
                    {std::int32_t{3}, rea_jackcess::DecimalValue{"-2341234.0345"}},
                    {std::int32_t{4}, rea_jackcess::DecimalValue{"0"}},
                    {std::int32_t{5}, std::monostate{}},
                });
                table->update_row(row_ids[0], {std::int32_t{1}, rea_jackcess::DecimalValue{"-5.5"}});
                table->delete_row(row_ids[4]);
                std::cout << "Inserted, updated and deleted MONEY rows with an indexed key\n";
            } else if (index_maintenance) {
                const auto row_ids = table->add_rows({
                    {std::int32_t{1}, std::int32_t{30}, std::string("a")},
                    {std::int32_t{2}, std::int32_t{10}, std::string("b")},
                    {std::int32_t{3}, std::int32_t{20}, std::string("c")},
                });
                table->update_row(row_ids[0], {std::int32_t{1}, std::int32_t{25}, std::string("a")});
                table->delete_row(row_ids[1]);
                std::cout << "Inserted, updated and deleted indexed rows\n";
            } else if (boolean_rows) {
                const auto row_ids = table->add_rows({
                    {std::int32_t{1}, true, false, std::int32_t{10}},
                    {std::int32_t{2}, false, true, std::int32_t{-5}},
                    {std::int32_t{3}, std::monostate{}, std::monostate{}, std::int32_t{0}},
                });
                table->update_row(row_ids.front(),
                                  {std::int32_t{1}, false, false, std::int32_t{11}});
                std::cout << "Wrote and updated Jet4 BOOLEAN bit values, including a NULL row\n";
            } else if (variable_rows) {
                const auto row_ids = table->add_rows({
                    {std::int32_t{1}, std::string("A"), rea_jackcess::BlobValue{{0, 0x7f, 0xff}}, std::string("🔥")},
                    {std::int32_t{2}, std::string{}, rea_jackcess::BlobValue{}, std::monostate{}},
                    {std::int32_t{3}, std::monostate{}, rea_jackcess::BlobValue{{0x80}}, std::string("雪")},
                    {std::int32_t{4}, std::string(255, 'x'),
                     rea_jackcess::BlobValue{std::vector<std::uint8_t>(255, 0xa5)}, std::string(255, 'y')},
                });
                if (variable_row_shrink) {
                    table->update_row(row_ids.front(),
                                      {std::int32_t{10}, std::string{},
                                       rea_jackcess::BlobValue{{1, 2, 3}}, std::string("🚀")});
                }
                table->update_row(row_ids.front(),
                                  {std::int32_t{10}, std::string(127, 'z'),
                                   rea_jackcess::BlobValue{{1, 2, 3}}, std::string("🚀")});
                table->update_row(row_ids.front(),
                                  {std::int32_t{10}, std::string(255, 'q'),
                                   rea_jackcess::BlobValue{{1, 2, 3}}, std::string("🚀")});
                table->update_row(row_ids.front(),
                                  {std::int32_t{10}, std::string(255, 'r'),
                                   rea_jackcess::BlobValue{{1, 2, 3}}, std::string("🚀")});
                if (variable_row_shrink) {
                    table->update_row(row_ids.front(),
                                      {std::int32_t{10}, std::string("B"),
                                       rea_jackcess::BlobValue{{1, 2, 3}}, std::string("🚀")});
                }
                std::cout << "Wrote UTF-8 TEXT and BINARY rows, including overflow-row growth"
                          << (variable_row_shrink ? " and shrink" : "") << ", empty, and NULL values\n";
            } else if (fixed_numeric_rows) {
                const auto row_ids = table->add_rows({
                    {std::int8_t{-128}, std::int16_t{-32768}, std::int32_t{-2147483647 - 1}, -1.25F, 1.0 / 3.0},
                    {std::int8_t{127}, std::int16_t{32767}, std::int32_t{2147483647}, 3.5F, 2.5},
                    {std::int8_t{1}, std::int16_t{-10}, std::int32_t{100}, 0.25F, 42.0},
                });
                table->update_row(row_ids.back(),
                                  {std::int8_t{-1}, std::int16_t{0}, std::int32_t{123456}, 12.5F, -0.5});
                std::cout << "Wrote fixed BYTE, SHORT, LONG, SINGLE, and DOUBLE rows\n";
            } else if (batch_rows) {
                std::vector<std::vector<rea_jackcess::Value>> rows;
                rows.reserve(500);
                for (std::int32_t value = 0; value < 500; ++value) {
                    rows.push_back({value, value + 1000});
                }
                const auto row_ids = table->add_rows(rows);
                if (row_ids.size() != rows.size()) {
                    throw std::runtime_error("Batch insertion returned the wrong RowId count");
                }
                (void)table->add_row({std::int32_t{501}, std::monostate{}});
                std::cout << "Inserted 500 rows across Jet4 data pages and one nullable row\n";
            } else if (update_row) {
                const auto row_id = table->add_row({std::int32_t{10}, std::int32_t{20}});
                table->update_row(row_id, {std::int32_t{11}, std::monostate{}});
                std::cout << "Updated one fixed-width Jet4 row in place\n";
            } else {
                const auto row_ids = table->add_rows({
                    {std::int32_t{1}, std::int32_t{10}},
                    {std::int32_t{2}, std::int32_t{20}},
                    {std::int32_t{3}, std::int32_t{30}},
                });
                table->delete_row(row_ids[1]);
                std::cout << "Deleted one Jet4 row and retained the surrounding rows\n";
            }
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Table creation fixture failed: " << error.what() << '\n';
        return 1;
    }
}
