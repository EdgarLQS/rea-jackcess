#include "rea_jackcess/database.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using rea_jackcess::Database;
using rea_jackcess::DatabaseError;
using rea_jackcess::DatabaseErrorCode;
using rea_jackcess::DatabaseFormat;
using rea_jackcess::DatabaseOpenMode;
using rea_jackcess::IndexBuilder;
using rea_jackcess::IndexColumnBuilder;
using rea_jackcess::RelationshipBuilder;
using rea_jackcess::Table;
using rea_jackcess::TableBuilder;
using rea_jackcess::Value;

void check(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

class TemporaryDirectory {
public:
    TemporaryDirectory() {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
                ("rea-jackcess-relationship-" + std::to_string(stamp));
        std::filesystem::create_directories(path_);
    }

    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

Table get_table(const Database& database, const std::string& name) {
    const auto tables = database.tables();
    const auto found = std::find_if(tables.begin(), tables.end(), [&](const auto& table) {
        return table.name() == name;
    });
    check(found != tables.end(), "table is missing: " + name);
    return *found;
}

std::vector<Value> long_key(std::int32_t first, std::int32_t second) {
    return {first, second};
}

void expect_code(DatabaseErrorCode expected, const std::string& label,
                 const std::function<void()>& operation) {
    try {
        operation();
    } catch (const DatabaseError& error) {
        check(error.code() == expected, label + ": unexpected error code for " + error.what());
        return;
    }
    throw std::runtime_error(label + ": the call was accepted");
}

void expect_invalid_argument(const std::string& label, const std::function<void()>& operation) {
    try {
        operation();
    } catch (const std::invalid_argument&) {
        return;
    } catch (const std::exception& error) {
        throw std::runtime_error(label + ": unexpected exception for " + error.what());
    }
    throw std::runtime_error(label + ": the call was accepted");
}

IndexBuilder composite_key(const std::string& name) {
    return IndexBuilder(name, {IndexColumnBuilder{"Region", true}, IndexColumnBuilder{"Code", true}});
}

IndexBuilder composite_foreign_key(const std::string& name) {
    return IndexBuilder(name, {IndexColumnBuilder{"ForeignRegion", true},
                               IndexColumnBuilder{"ForeignCode", true}});
}

// Builds the tables a relationship needs: a parent with a composite primary key, a child with a
// composite index over the foreign key columns, and a few tables that must be rejected.
void create_schema(const std::filesystem::path& path, DatabaseFormat format) {
    Database::create(path, format,
                     {TableBuilder("Parent")
                          .add_column("Region", rea_jackcess::DataType::int32)
                          .add_column("Code", rea_jackcess::DataType::int32)
                          .add_index(composite_key("PrimaryKey").with_primary_key()),
                      TableBuilder("Child")
                          .add_column("ForeignRegion", rea_jackcess::DataType::int32)
                          .add_column("ForeignCode", rea_jackcess::DataType::int32)
                          .add_index(composite_foreign_key("ChildFk")),
                      TableBuilder("LooseParent")
                          .add_column("Region", rea_jackcess::DataType::int32)
                          .add_column("Code", rea_jackcess::DataType::int32)
                          .add_index(composite_key("LooseIndex")),
                      TableBuilder("UnindexedChild")
                          .add_column("ForeignRegion", rea_jackcess::DataType::int32)
                          .add_column("ForeignCode", rea_jackcess::DataType::int32),
                      TableBuilder("OtherChild")
                          .add_column("ForeignRegion", rea_jackcess::DataType::int16)
                          .add_column("ForeignCode", rea_jackcess::DataType::int32)
                          .add_index(IndexBuilder("OtherFk",
                                                  {IndexColumnBuilder{"ForeignRegion", true},
                                                   IndexColumnBuilder{"ForeignCode", true}}))});
}

RelationshipBuilder cascading_relationship() {
    return RelationshipBuilder("ParentChild", "Parent", "Child")
        .add_columns("Region", "ForeignRegion")
        .add_columns("Code", "ForeignCode")
        .with_referential_integrity()
        .with_cascade_updates()
        .with_cascade_deletes();
}

std::vector<char> read_bytes(const std::filesystem::path& path);

void check_created_indexes(const Database& database) {
    const auto parent = get_table(database, "Parent");
    check(parent.indexes().size() == 2, "the parent should gain a relationship alias index");
    const auto alias = std::find_if(parent.indexes().begin(), parent.indexes().end(),
                                    [](const auto& index) { return index.foreign_key; });
    check(alias != parent.indexes().end(), "the parent alias should be a foreign key description");
    check(alias->columns.size() == 2 && alias->cascade_updates && alias->cascade_deletes,
          "the parent alias should describe the composite cascading relationship");
    check(alias->physical_index_number == parent.indexes().front().physical_index_number,
          "the alias should reuse the primary index data");

    const auto child = get_table(database, "Child");
    const auto foreign = std::find_if(child.indexes().begin(), child.indexes().end(),
                                      [](const auto& index) { return index.foreign_key; });
    check(foreign != child.indexes().end(), "the child should gain a foreign key description");
    check(foreign->name == "ParentChild", "the child foreign key should carry the relationship name");
    check(foreign->columns.size() == 2 && foreign->cascade_updates && foreign->cascade_deletes,
          "the child foreign key should describe the cascading relationship");
    check(foreign->referenced_table_page_number.has_value(),
          "the child foreign key should point at the parent table");
}

void check_rejections(Database& database) {
    // A relationship without referential integrity only adds catalog rows.
    database.create_relationship(RelationshipBuilder("LooseJoin", "Parent", "Child")
                                     .add_columns("Region", "ForeignRegion")
                                     .add_columns("Code", "ForeignCode")
                                     .with_referential_integrity(false)
                                     .with_join_type(rea_jackcess::RelationshipJoinType::left_outer));
    const auto& joined = database.relationships();
    const auto loose = std::find_if(joined.begin(), joined.end(), [](const auto& item) {
        return item.name == "LooseJoin";
    });
    check(loose != joined.end(), "the relationship without referential integrity is missing");
    check(!loose->referential_integrity, "the loose relationship should not enforce integrity");
    check(loose->join_type == rea_jackcess::RelationshipJoinType::left_outer,
          "the loose relationship should keep its join type");
    check(get_table(database, "Child").indexes().size() == 2,
          "a relationship without integrity should not add an index description");

    expect_invalid_argument("parent without a unique index", [&] {
        database.create_relationship(RelationshipBuilder("LooseParentJoin", "LooseParent", "Child")
                                         .add_columns("Region", "ForeignRegion")
                                         .add_columns("Code", "ForeignCode")
                                         .with_referential_integrity());
    });
    expect_invalid_argument("unknown column", [&] {
        database.create_relationship(RelationshipBuilder("UnknownColumn", "Parent", "Child")
                                         .add_columns("Region", "ForeignRegion")
                                         .add_columns("Missing", "ForeignCode")
                                         .with_referential_integrity());
    });
    expect_invalid_argument("column type mismatch", [&] {
        database.create_relationship(RelationshipBuilder("TypeMismatch", "Parent", "OtherChild")
                                         .add_columns("Region", "ForeignRegion")
                                         .add_columns("Code", "ForeignCode")
                                         .with_referential_integrity());
    });
    expect_invalid_argument("cascade without integrity", [&] {
        database.create_relationship(RelationshipBuilder("CascadeWithoutIntegrity", "Parent", "Child")
                                         .add_columns("Region", "ForeignRegion")
                                         .add_columns("Code", "ForeignCode")
                                         .with_referential_integrity(false)
                                         .with_cascade_deletes());
    });
    expect_invalid_argument("unknown table", [&] {
        database.create_relationship(RelationshipBuilder("UnknownTable", "Missing", "Child")
                                         .add_columns("Id", "ForeignRegion"));
    });
    expect_invalid_argument("invalid relationship identifier", [&] {
        database.create_relationship(RelationshipBuilder("Bad.Name", "Parent", "Child")
                                         .add_columns("Region", "ForeignRegion"));
    });
    expect_invalid_argument("leading-space relationship identifier", [&] {
        database.create_relationship(RelationshipBuilder(" BadName", "Parent", "Child")
                                         .add_columns("Region", "ForeignRegion"));
    });
    expect_invalid_argument("empty column mapping", [&] {
        database.create_relationship(RelationshipBuilder("EmptyMapping", "Parent", "Child"));
    });
    expect_invalid_argument("duplicate child columns", [&] {
        database.create_relationship(RelationshipBuilder("DuplicateChildColumns", "Parent", "Child")
                                         .add_columns("Region", "ForeignRegion")
                                         .add_columns("Code", "ForeignRegion")
                                         .with_referential_integrity());
    });
    const auto before_invalid_join = read_bytes(database.path());
    expect_invalid_argument("invalid join type", [&] {
        database.create_relationship(
            RelationshipBuilder("InvalidJoinType", "Parent", "Child")
                .add_columns("Region", "ForeignRegion")
                .with_referential_integrity(false)
                .with_join_type(static_cast<rea_jackcess::RelationshipJoinType>(99)));
    });
    check(read_bytes(database.path()) == before_invalid_join,
          "an invalid join type changed the database");
}

void test_combined_delete_actions(const std::filesystem::path& path, DatabaseFormat format) {
    create_schema(path, format);
    auto database = Database::open(path, DatabaseOpenMode::read_write);
    const auto relationship = RelationshipBuilder("DeleteAndNull", "Parent", "Child")
                                  .add_columns("Region", "ForeignRegion")
                                  .add_columns("Code", "ForeignCode")
                                  .with_referential_integrity()
                                  .with_cascade_deletes()
                                  .with_cascade_null_on_delete();
    database.create_relationship(relationship);
    const auto created = database.relationships();
    const auto item = std::find_if(created.begin(), created.end(), [](const auto& value) {
        return value.name == "DeleteAndNull";
    });
    check(item != created.end() && item->cascade_deletes && item->cascade_null_on_delete,
          "the relationship did not preserve both delete action flags");

    auto reopened = Database::open(path);
    const auto persisted = reopened.relationships();
    const auto saved = std::find_if(persisted.begin(), persisted.end(), [](const auto& value) {
        return value.name == "DeleteAndNull";
    });
    check(saved != persisted.end() && saved->cascade_deletes && saved->cascade_null_on_delete,
          "both delete action flags did not survive reopening");
}

void test_unenforced_relationship_many_columns(const std::filesystem::path& path, DatabaseFormat format) {
    TableBuilder parent("ManyParent");
    TableBuilder child("ManyChild");
    RelationshipBuilder relationship("ManyColumnJoin", "ManyParent", "ManyChild");
    for (std::size_t column = 0; column < 11; ++column) {
        const auto suffix = std::to_string(column);
        parent.add_column("Parent" + suffix, rea_jackcess::DataType::int32);
        child.add_column("Child" + suffix, rea_jackcess::DataType::int32);
        relationship.add_columns("Parent" + suffix, "Child" + suffix);
    }
    relationship.with_referential_integrity(false);
    Database::create(path, format, {parent, child});
    {
        auto database = Database::open(path, DatabaseOpenMode::read_write);
        database.create_relationship(relationship);
        const auto created = database.relationships();
        check(created.size() == 1 && !created.front().referential_integrity &&
                  created.front().columns.size() == 11,
              "an unenforced relationship with more than ten column pairs was not retained");
        check(get_table(database, "ManyParent").indexes().empty() &&
                  get_table(database, "ManyChild").indexes().empty(),
              "an unenforced multi-column relationship unexpectedly added indexes");
    }
    const auto reopened = Database::open(path);
    const auto relationships = reopened.relationships();
    check(relationships.size() == 1 && relationships.front().columns.size() == 11,
          "an unenforced relationship with more than ten column pairs did not survive reopening");
}

std::vector<char> read_bytes(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    check(stream.is_open(), "could not open test database for byte comparison");
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

void test_automatic_child_index(const std::filesystem::path& path, DatabaseFormat format) {
    create_schema(path, format);
    {
        auto database = Database::open(path, DatabaseOpenMode::read_write);
        auto parent = get_table(database, "Parent");
        auto child = get_table(database, "UnindexedChild");
        const auto child_id = child.add_row(long_key(3, 4));
        const auto relationship = RelationshipBuilder("AutoChildIndex", "parent", "unindexedchild")
                                      .add_columns("Region", "ForeignRegion")
                                      .add_columns("Code", "ForeignCode")
                                      .with_referential_integrity()
                                      .with_cascade_updates();
        const auto before = read_bytes(path);
        expect_code(DatabaseErrorCode::constraint_violation, "unindexed orphan child", [&] {
            database.create_relationship(relationship);
        });
        check(read_bytes(path) == before && child.indexes().empty(),
              "rejected relationship allocated an index or changed the database");

        const auto parent_id = parent.add_row(long_key(3, 4));
        database.create_relationship(relationship);
        check(child.indexes().size() == 1 && child.indexes().front().foreign_key &&
                  parent.indexes().size() == 2,
              "the relationship should expose both new index descriptors immediately");
        const auto row = child.find_row_by_index("AutoChildIndex", long_key(3, 4));
        check(row && row->id() == child_id, "new child index did not backfill existing rows");
        parent.update_row(parent_id, long_key(3, 5));
        check(child.find_row_by_index("AutoChildIndex", long_key(3, 5)).has_value(),
              "cascade did not maintain the automatically created index");
    }
    auto reopened = Database::open(path);
    check(reopened.relationships().size() == 1 &&
          get_table(reopened, "UnindexedChild")
              .find_row_by_index("AutoChildIndex", long_key(3, 5)).has_value(),
          "relationship or automatic index did not survive reopen");
}

void test_cascade_cycle(const std::filesystem::path& path, DatabaseFormat format) {
    std::vector<TableBuilder> tables;
    for (const auto* name : {"NodeA", "NodeB", "NodeC"}) {
        tables.push_back(TableBuilder(name)
                             .add_column("Id", rea_jackcess::DataType::int32)
                             .add_index(IndexBuilder("PK", "Id").with_primary_key()));
    }
    Database::create(path, format, tables);
    auto database = Database::open(path, DatabaseOpenMode::read_write);
    const auto relation = [](const char* name, const char* from, const char* to) {
        return RelationshipBuilder(name, from, to)
            .add_columns("Id", "Id")
            .with_referential_integrity()
            .with_cascade_updates();
    };
    database.create_relationship(relation("AToB", "NodeA", "NodeB"));
    database.create_relationship(relation("BToC", "NodeB", "NodeC"));

    const auto before = read_bytes(path);
    expect_invalid_argument("transitive cascade cycle", [&] {
        database.create_relationship(relation("CToA", "nodec", "nodea"));
    });
    check(read_bytes(path) == before, "rejected relationship changed the database");

    database.create_relationship(relation("atob", "NodeC", "NodeA").with_cascade_updates(false));
    database.create_relationship(relation("PK", "NodeC", "NodeA").with_cascade_updates(false));
    database.create_relationship(RelationshipBuilder("", "nodec", "nodea")
                                     .add_columns("Id", "Id").with_referential_integrity(false));
    const auto relationships = database.relationships();
    const auto contains = [&](const std::string& name) {
        return std::any_of(relationships.begin(), relationships.end(),
                           [&](const auto& item) { return item.name == name; });
    };
    check(relationships.size() == 5 && contains("atob1") && contains("PK1") && contains("NodeCNodeA"),
          "relationship names did not follow collision and default-name rules");
    for (const auto& item : relationships) {
        check(item.one_to_one && (item.raw_flags & 1) != 0,
              "matching unique indexes must set the one-to-one flag, even without enforcement");
    }
    const auto reopened = Database::open(path).relationships();
    check(reopened.size() == relationships.size() &&
              std::all_of(reopened.begin(), reopened.end(), [](const auto& item) { return item.one_to_one; }),
          "the one-to-one flag did not survive reopen");
    const auto child = get_table(database, "NodeA");
    for (const auto* name : {"atob1", "PK1"}) {
        check(std::any_of(child.indexes().begin(), child.indexes().end(),
                          [&](const auto& index) { return index.foreign_key && index.name == name; }),
              "the foreign key index did not receive the final relationship name");
    }
}

void test_set_null_relationship_is_not_cycle_edge(const std::filesystem::path& path,
                                                 DatabaseFormat format) {
    std::vector<TableBuilder> tables;
    for (const auto* name : {"NodeA", "NodeB", "NodeC"}) {
        tables.push_back(TableBuilder(name)
                             .add_column("Id", rea_jackcess::DataType::int32)
                             .add_index(IndexBuilder("PK", "Id").with_primary_key()));
    }
    Database::create(path, format, tables);
    auto database = Database::open(path, DatabaseOpenMode::read_write);
    const auto relation = [](const char* name, const char* from, const char* to) {
        return RelationshipBuilder(name, from, to)
            .add_columns("Id", "Id")
            .with_referential_integrity();
    };
    database.create_relationship(
        relation("ASetNullB", "NodeA", "NodeB").with_cascade_null_on_delete());
    database.create_relationship(
        relation("BToC", "NodeB", "NodeC").with_cascade_updates());
    database.create_relationship(
        relation("CToA", "NodeC", "NodeA").with_cascade_updates());
    check(database.relationships().size() == 3,
          "an existing set-null relationship incorrectly formed a cascade cycle");
}

void test_self_reference_relationship(const std::filesystem::path& path, DatabaseFormat format) {
    Database::create(path, format,
                     {TableBuilder("Employees")
                          .add_column("Id", rea_jackcess::DataType::int32)
                          .add_column("ManagerId", rea_jackcess::DataType::int32)
                          .add_index(IndexBuilder("PK_Employees", "Id").with_primary_key())});
    {
        auto database = Database::open(path, DatabaseOpenMode::read_write);
        auto employees = get_table(database, "Employees");
        const auto rows = employees.add_rows({{std::int32_t{1}, std::monostate{}},
                                              {std::int32_t{2}, std::int32_t{1}}});
        check(rows.size() == 2, "self-reference fixture rows were not added");

        const auto orphan = employees.add_row({std::int32_t{3}, std::int32_t{99}});
        const auto before_orphan_rejection = read_bytes(path);
        expect_code(DatabaseErrorCode::constraint_violation, "existing self-reference orphan", [&] {
            database.create_relationship(
                RelationshipBuilder("InvalidSelf", "Employees", "Employees")
                    .add_columns("Id", "ManagerId")
                    .with_referential_integrity());
        });
        check(read_bytes(path) == before_orphan_rejection,
              "a rejected self-reference definition changed the database");
        employees.delete_row(orphan);

        const auto before_cycle_rejections = read_bytes(path);
        const auto self_cascade = [](RelationshipBuilder relationship) {
            return relationship.add_columns("Id", "ManagerId").with_referential_integrity();
        };
        expect_invalid_argument("self cascade update", [&] {
            database.create_relationship(
                self_cascade(RelationshipBuilder("SelfUpdate", "Employees", "Employees"))
                    .with_cascade_updates());
        });
        expect_invalid_argument("self cascade delete", [&] {
            database.create_relationship(
                self_cascade(RelationshipBuilder("SelfDelete", "Employees", "Employees"))
                    .with_cascade_deletes());
        });
        expect_invalid_argument("self cascade set-null", [&] {
            database.create_relationship(
                self_cascade(RelationshipBuilder("SelfNull", "Employees", "Employees"))
                    .with_cascade_null_on_delete());
        });
        check(read_bytes(path) == before_cycle_rejections,
              "rejected self-cascading relationships modified the database");

        database.create_relationship(
            self_cascade(RelationshipBuilder("SelfRelation", "Employees", "Employees")));
        const auto relationships = database.relationships();
        check(relationships.size() == 1 && relationships.front().name == "SelfRelation" &&
                  relationships.front().from_table == "Employees" &&
                  relationships.front().to_table == "Employees" &&
                  relationships.front().referential_integrity,
              "non-cascading self-reference relationship metadata was not created");

        employees = get_table(database, "Employees");
        const auto indexes = employees.indexes();
        const auto foreign_index = std::find_if(
            indexes.begin(), indexes.end(), [](const auto& index) {
                return index.name == "SelfRelation" && index.foreign_key;
            });
        const auto parent_alias = std::find_if(
            indexes.begin(), indexes.end(), [](const auto& index) {
                return index.name == ".rB" && index.foreign_key;
            });
        check(foreign_index != indexes.end() &&
                  parent_alias != indexes.end() &&
                  foreign_index->referenced_table_page_number == parent_alias->referenced_table_page_number,
              "self-reference indexes did not receive distinct reciprocal descriptors");
        const auto initial_child = employees.find_row_by_index("SelfRelation", std::int32_t{1});
        const auto initial_parent = employees.find_row_by_index(".rB", std::int32_t{1});
        check(initial_child.has_value() && initial_child->id() == rows[1] &&
                  initial_parent.has_value() && initial_parent->id() == rows[0],
              "self-reference indexes did not point to their respective parent and child rows");

        const auto child = employees.add_row({std::int32_t{3}, std::int32_t{1}});
        bool found_child_in_index = false;
        for (auto cursor = employees.rows_by_index(".rB"); auto row = cursor.next();) {
            found_child_in_index = found_child_in_index || row->id() == child;
        }
        check(found_child_in_index, "a valid self-referencing child was not indexed");
        const auto before_orphan = read_bytes(path);
        expect_code(DatabaseErrorCode::constraint_violation, "self-reference orphan insert", [&] {
            employees.add_row({std::int32_t{4}, std::int32_t{99}});
        });
        check(read_bytes(path) == before_orphan,
              "a rejected self-referencing orphan modified the database");
        expect_code(DatabaseErrorCode::constraint_violation, "self-reference parent delete", [&] {
            employees.delete_row(rows[0]);
        });
        bool child_still_present = false;
        for (auto cursor = employees.rows_by_index(".rB"); auto row = cursor.next();) {
            child_still_present = child_still_present || row->id() == child;
        }
        const auto parent_after_rejected_delete = employees.find_row_by_index(".rB", std::int32_t{1});
        check(child_still_present && parent_after_rejected_delete.has_value() &&
                  parent_after_rejected_delete->id() == rows[0],
              "a rejected self-referencing delete changed the child index");
    }

    const auto reopened = Database::open(path);
    const auto employees = get_table(reopened, "Employees");
    check(reopened.relationships().size() == 1 && employees.indexes().size() == 3 &&
              employees.find_row_by_index(".rB", std::int32_t{1}).has_value() &&
              employees.find_row_by_index("SelfRelation", std::int32_t{1}).has_value(),
          "self-reference descriptors or indexes did not survive reopening");
}

void test_relationship_rollback(const std::filesystem::path& path, DatabaseFormat format) {
    create_schema(path, format);
    const std::string version = format == DatabaseFormat::v2000 ? "08.50" : "09.50";
    std::vector<char> marker;
    for (const auto character : version) {
        marker.push_back(character);
        marker.push_back(0);
    }
    const auto original = read_bytes(path);
    {
        // V2003 contains two copies of the version marker. Change both in this disposable
        // fixture so the catalog row writer rejects the version after the schema batch.
        std::fstream stream(path, std::ios::binary | std::ios::in | std::ios::out);
        auto match = std::search(original.begin(), original.end(), marker.begin(), marker.end());
        check(match != original.end(), "fixture is missing its AccessVersion marker");
        while (match != original.end()) {
            stream.seekp(static_cast<std::streamoff>(match - original.begin()));
            stream.put('9');
            match = std::search(match + marker.size(), original.end(), marker.begin(), marker.end());
        }
        stream.close();
        check(static_cast<bool>(stream), "could not set the fixture's unknown AccessVersion");
    }

    const auto before = read_bytes(path);
    auto database = Database::open(path, DatabaseOpenMode::read_write);
    const auto relationship = RelationshipBuilder("RollbackFK", "Parent", "UnindexedChild")
                                  .add_columns("Region", "ForeignRegion")
                                  .add_columns("Code", "ForeignCode").with_referential_integrity();
    for (int attempt = 0; attempt < 2; ++attempt) {
        bool rejected_by_catalog_writer = false;
        try {
            database.create_relationship(relationship);
        } catch (const DatabaseError& error) {
            rejected_by_catalog_writer = error.code() == DatabaseErrorCode::unsupported_operation &&
                                         error.context().table_name == "MSysRelationships";
        }
        check(rejected_by_catalog_writer, "expected the relationship catalog write to reject the version");
        check(read_bytes(path) == before, "relationship rollback changed the fixture or retained appended pages");
        check(database.relationships().empty() && get_table(database, "Parent").indexes().size() == 1 &&
                  get_table(database, "UnindexedChild").indexes().empty(),
              "relationship rollback left stale metadata in the live handle");
    }
}

void test_cascade_rollback(const std::filesystem::path& path, DatabaseFormat format) {
    using rea_jackcess::DataType;
    Database::create(path, format,
                     {TableBuilder("Parent").add_column("Id", DataType::int32)
                                            .add_index(IndexBuilder("PK", "Id").with_primary_key()),
                      TableBuilder("OptionalChild").add_column("Fk", DataType::int32),
                      TableBuilder("RequiredChild").add_column("Fk", DataType::int32)
                                                    .add_index(IndexBuilder("RequiredFk", "Fk").with_required())});
    auto database = Database::open(path, DatabaseOpenMode::read_write);
    auto parent = get_table(database, "Parent");
    auto optional = get_table(database, "OptionalChild");
    auto required = get_table(database, "RequiredChild");
    const auto parent_id = parent.add_row({std::int32_t{9}});
    optional.add_row({std::int32_t{9}});
    const auto required_id = required.add_row({std::int32_t{9}});
    for (const auto* table : {"OptionalChild", "RequiredChild"}) {
        database.create_relationship(RelationshipBuilder(table, "Parent", table)
                                         .add_columns("Id", "Fk").with_referential_integrity()
                                         .with_cascade_null_on_delete());
    }

    const auto before = read_bytes(path);
    expect_code(DatabaseErrorCode::unsupported_operation, "cascade attempts to clear a required index", [&] {
        parent.delete_row(parent_id);
    });
    check(read_bytes(path) == before, "failed cascade did not restore the database byte for byte");
    check(parent.row_count_header() == 1 &&
              optional.find_row_by_index("OptionalChild", std::int32_t{9}).has_value(),
          "failed cascade left the live handle out of sync");

    required.delete_row(required_id);
    parent.delete_row(parent_id);
    check(parent.row_count_header() == 0 &&
              optional.find_row_by_index("OptionalChild", std::monostate{}).has_value(),
          "cascade retry did not succeed after removing the conflicting row");
}

void test_creation(const std::filesystem::path& path, DatabaseFormat format) {
    create_schema(path, format);
    {
        auto database = Database::open(path, DatabaseOpenMode::read_write);
        auto parent = get_table(database, "Parent");
        auto child = get_table(database, "Child");
        const auto null_child = child.add_row({std::monostate{}, std::monostate{}});
        const auto orphan = child.add_row(long_key(1, 2));
        const auto before = read_bytes(path);
        expect_code(DatabaseErrorCode::constraint_violation, "existing orphan child row", [&] {
            database.create_relationship(cascading_relationship());
        });
        check(read_bytes(path) == before, "rejected relationship changed the database");
        check(database.relationships().empty(), "rejected relationship appeared in the catalog");
        const auto parent_row = parent.add_row(long_key(1, 2));
        database.create_relationship(cascading_relationship());
        check_created_indexes(database);
        const auto relationships = database.relationships();
        check(relationships.size() == 1 && !relationships.front().one_to_one,
              "a non-unique child index should produce a one-to-many relationship");

        check(child.row_count_header() == 2, "the all-NULL child row should survive creation");
        child.delete_row(null_child);
        expect_code(DatabaseErrorCode::constraint_violation, "orphan child row", [&] {
            child.add_row(long_key(9, 9));
        });

        // The created relationship has to behave like a relationship read from a Jackcess file.
        parent.update_row(parent_row, long_key(1, 5));
        const auto updated = child.find_row_by_index("ParentChild", long_key(1, 5));
        check(updated && updated->id() == orphan,
              "the created relationship did not cascade the key change");
        parent.delete_row(parent_row);
        check(child.row_count_header() == 0, "the created relationship did not cascade the delete");

        check_rejections(database);
    }

    // Reopening proves the descriptors and catalog rows were written, not just cached.
    auto database = Database::open(path, DatabaseOpenMode::read_only);
    const auto relationships = database.relationships();
    check(relationships.size() == 2, "both relationships should survive a reopen");
    const auto created = std::find_if(relationships.begin(), relationships.end(), [](const auto& item) {
        return item.name == "ParentChild";
    });
    check(created != relationships.end(), "the cascading relationship is missing after a reopen");
    check(created->from_table == "Parent" && created->to_table == "Child" &&
              created->columns.size() == 2,
          "the reopened relationship has the wrong endpoints");
    check(created->referential_integrity && created->cascade_updates && created->cascade_deletes,
          "the reopened relationship lost its referential flags");
    check_created_indexes(database);
}

}

int main() {
    try {
        TemporaryDirectory directory;
        test_creation(directory.path() / "relationship-v2000.mdb", DatabaseFormat::v2000);
        test_creation(directory.path() / "relationship-v2003.mdb", DatabaseFormat::v2003);
        test_combined_delete_actions(directory.path() / "combined-delete-actions-v2000.mdb",
                                     DatabaseFormat::v2000);
        test_combined_delete_actions(directory.path() / "combined-delete-actions-v2003.mdb",
                                     DatabaseFormat::v2003);
        test_unenforced_relationship_many_columns(directory.path() / "many-columns-v2000.mdb",
                                                  DatabaseFormat::v2000);
        test_unenforced_relationship_many_columns(directory.path() / "many-columns-v2003.mdb",
                                                  DatabaseFormat::v2003);
        test_automatic_child_index(directory.path() / "auto-index-v2000.mdb", DatabaseFormat::v2000);
        test_automatic_child_index(directory.path() / "auto-index-v2003.mdb", DatabaseFormat::v2003);
        test_cascade_cycle(directory.path() / "cycle-v2000.mdb", DatabaseFormat::v2000);
        test_cascade_cycle(directory.path() / "cycle-v2003.mdb", DatabaseFormat::v2003);
        test_set_null_relationship_is_not_cycle_edge(
            directory.path() / "set-null-cycle-v2000.mdb", DatabaseFormat::v2000);
        test_set_null_relationship_is_not_cycle_edge(
            directory.path() / "set-null-cycle-v2003.mdb", DatabaseFormat::v2003);
        test_self_reference_relationship(directory.path() / "self-reference-v2000.mdb",
                                         DatabaseFormat::v2000);
        test_self_reference_relationship(directory.path() / "self-reference-v2003.mdb",
                                         DatabaseFormat::v2003);
        test_cascade_rollback(directory.path() / "rollback-v2000.mdb", DatabaseFormat::v2000);
        test_cascade_rollback(directory.path() / "rollback-v2003.mdb", DatabaseFormat::v2003);
        test_relationship_rollback(directory.path() / "catalog-rollback-v2000.mdb", DatabaseFormat::v2000);
        test_relationship_rollback(directory.path() / "catalog-rollback-v2003.mdb", DatabaseFormat::v2003);
        std::cout << "relationship tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "relationship tests failed: " << error.what() << '\n';
        return 1;
    }
}
