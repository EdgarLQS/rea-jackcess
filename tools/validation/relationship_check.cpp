#include "rea_jackcess/database.hpp"

#include <algorithm>
#include <iostream>
#include <stdexcept>

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "Usage: relationship_check <database.mdb>\n";
        return 2;
    }
    try {
        const auto database = rea_jackcess::Database::open(argv[1]);
        const auto relationships = database.relationships();
        const auto find_relationship = [&](const char* name) -> const rea_jackcess::RelationshipInfo& {
            const auto found = std::find_if(relationships.begin(), relationships.end(), [&](const auto& item) {
                return item.name == name;
            });
            if (found == relationships.end()) {
                throw std::runtime_error("Expected relationship was missing");
            }
            return *found;
        };
        if (relationships.size() != 2) {
            throw std::runtime_error("Expected two user relationships");
        }
        const auto& relationship = find_relationship("ParentChild");
        if (relationship.name != "ParentChild" || relationship.from_table != "Parent" ||
            relationship.to_table != "Child" || relationship.columns.size() != 2 ||
            relationship.columns[0].from_column != "Region" ||
            relationship.columns[0].to_column != "ForeignRegion" ||
            relationship.columns[1].from_column != "Code" ||
            relationship.columns[1].to_column != "ForeignCode" ||
            relationship.one_to_one || !relationship.referential_integrity ||
            !relationship.cascade_updates || !relationship.cascade_deletes ||
            relationship.cascade_null_on_delete ||
            relationship.join_type != rea_jackcess::RelationshipJoinType::left_outer) {
            throw std::runtime_error("Relationship metadata differs from the Jackcess fixture");
        }
        const auto& join = find_relationship("ReadOnlyJoin");
        if (join.referential_integrity || join.columns.size() != 3 ||
            join.join_type != rea_jackcess::RelationshipJoinType::right_outer ||
            join.columns[0].from_column != "Code" || join.columns[1].from_column != "Region" ||
            join.columns[2].to_column != "ForeignCode") {
            throw std::runtime_error("Unenforced relationship metadata differs from the Jackcess fixture");
        }
        std::cout << "Verified the enforced relationship and unenforced relationship metadata\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
