#include <rea_jackcess/database.hpp>
#include <cstdlib>
#include <string>
#include <vector>

using namespace rea_jackcess;

int main(int argc, char** argv) {
    if (argc != 3) return 2;
    const auto format = std::string(argv[2]) == "v2003" ? DatabaseFormat::v2003 : DatabaseFormat::v2000;
    const std::filesystem::path path(argv[1]);
    TableBuilder many_parent("ManyParent");
    TableBuilder many_child("ManyChild");
    for (std::size_t column = 0; column < 11; ++column) {
        const auto suffix = std::to_string(column);
        many_parent.add_column("Parent" + suffix, DataType::int32);
        many_child.add_column("Child" + suffix, DataType::int32);
    }
    {
        auto created = Database::create(path, format,
            {TableBuilder("Parent")
                 .add_column("Id", DataType::int32)
                 .add_index(IndexBuilder("PK", "Id").with_primary_key()),
             TableBuilder("Child").add_column("ParentId", DataType::int32),
             many_parent,
             many_child});
    }
    auto database = Database::open(path, DatabaseOpenMode::read_write);
    database.create_relationship(RelationshipBuilder("DeleteAndNull", "Parent", "Child")
        .add_columns("Id", "ParentId")
        .with_referential_integrity()
        .with_cascade_deletes()
        .with_cascade_null_on_delete());
    RelationshipBuilder many_columns("ManyColumnJoin", "ManyParent", "ManyChild");
    for (std::size_t column = 0; column < 11; ++column) {
        const auto suffix = std::to_string(column);
        many_columns.add_columns("Parent" + suffix, "Child" + suffix);
    }
    database.create_relationship(many_columns.with_referential_integrity(false));
}
