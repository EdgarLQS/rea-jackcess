#include "database_internal.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <map>
#include <set>
#include <utility>

namespace rea_jackcess {
namespace {

std::string identifier_key(std::string name) {
    for (auto& character : name) {
        if (character >= 'A' && character <= 'Z') {
            character = static_cast<char>(character - 'A' + 'a');
        }
    }
    return name;
}

std::string custom_source_cache_key(const LinkedTableInfo& link) {
    std::string key(1, '\0');
    key.push_back(link.kind == LinkedTableKind::file ? 'f' : 'o');
    const auto target_database = link.target_database.value_or(std::string{});
    const auto connection_string = link.connection_string.value_or(std::string{});
    const auto append = [&](const std::string& part) {
        key += std::to_string(part.size());
        key.push_back(':');
        key += part;
    };
    append(target_database);
    append(connection_string);
    append(target_database.empty() && connection_string.empty() ? link.name : std::string{});
    return key;
}

std::string custom_table_cache_key(const std::string& source_key, const std::string& table_name) {
    std::string key(1, '\0');
    key.push_back('t');
    key += std::to_string(source_key.size());
    key.push_back(':');
    key += source_key;
    const auto normalized_name = identifier_key(table_name);
    key += std::to_string(normalized_name.size());
    key.push_back(':');
    key += normalized_name;
    return key;
}

std::shared_ptr<detail::TemporaryDatabaseDirectory> create_temporary_database_directory(
    const std::filesystem::path& source) {
    auto owner = std::make_shared<detail::TemporaryDatabaseDirectory>();
    std::error_code error;
    const auto base = std::filesystem::temp_directory_path(error);
    if (error) {
        throw DatabaseError(DatabaseErrorCode::io_error,
                            "Unable to locate a temporary directory for a custom linked table",
                            source);
    }

    static std::atomic<std::uint64_t> sequence{0};
    const auto timestamp = std::chrono::steady_clock::now().time_since_epoch().count();
    for (std::size_t attempt = 0; attempt < 128; ++attempt) {
        const auto suffix = std::to_string(sequence.fetch_add(1, std::memory_order_relaxed));
        const auto directory = base / ("rea-jackcess-linked-" + std::to_string(timestamp) + "-" + suffix);
        error.clear();
        if (std::filesystem::create_directory(directory, error)) {
            owner->path = directory;
            return owner;
        }
        if (error && error != std::errc::file_exists) {
            throw DatabaseError(DatabaseErrorCode::io_error,
                                "Unable to create a temporary directory for a custom linked table",
                                source);
        }
    }
    throw DatabaseError(DatabaseErrorCode::io_error,
                        "Unable to reserve a temporary directory for a custom linked table",
                        source);
}

std::pair<Database, std::shared_ptr<detail::TemporaryDatabaseDirectory>> materialize_custom_linked_table(
    const std::filesystem::path& source,
    const LinkedTableInfo& link,
    LinkedTableSnapshot snapshot,
    DatabaseOpenMode result_mode) {
    if (!link.target_table || identifier_key(snapshot.schema.name()) != identifier_key(*link.target_table)) {
        throw DatabaseError(DatabaseErrorCode::unsupported_operation,
                            "The custom linked-table schema name does not match its target table",
                            source);
    }

    auto directory = create_temporary_database_directory(source);
    const auto path = directory->path / "linked.mdb";
    {
        auto created = Database::create(path, DatabaseFormat::v2000, {snapshot.schema});
    }
    {
        auto writable = Database::open(path, DatabaseOpenMode::read_write);
        auto tables = writable.tables();
        const auto table = std::find_if(tables.begin(), tables.end(), [&](const auto& candidate) {
            return identifier_key(candidate.name()) == identifier_key(*link.target_table);
        });
        if (table == tables.end()) {
            throw DatabaseError(DatabaseErrorCode::unsupported_operation,
                                "The custom linked-table snapshot did not create its target table",
                                source);
        }
        constexpr std::size_t batch_size = 256;
        for (std::size_t begin = 0; begin < snapshot.rows.size(); begin += batch_size) {
            const auto end = std::min(snapshot.rows.size(), begin + batch_size);
            std::vector<std::vector<Value>> batch(snapshot.rows.begin() + static_cast<std::ptrdiff_t>(begin),
                                                  snapshot.rows.begin() + static_cast<std::ptrdiff_t>(end));
            table->add_rows(batch);
        }
    }

    return {Database::open(path, result_mode), std::move(directory)};
}

[[noreturn]] void invalid_relationship(const Database& database, const char* reason) {
    DatabaseErrorContext context;
    context.table_name = "MSysRelationships";
    throw DatabaseError(DatabaseErrorCode::corrupted_file, reason, database.path(),
                        std::nullopt, std::move(context));
}

std::size_t field_index(const Database& database, const Table& table, const std::string& name) {
    const auto key = identifier_key(name);
    const auto& columns = table.columns();
    const auto column = std::find_if(columns.begin(), columns.end(), [&](const ColumnInfo& candidate) {
        return identifier_key(candidate.name) == key;
    });
    if (column == columns.end()) {
        invalid_relationship(database, "Relationship references an unknown column");
    }
    return static_cast<std::size_t>(column - columns.begin());
}

template <typename Type>
const Type& relationship_field(const Database& database, const Row& row, std::size_t field) {
    const auto* value = std::get_if<Type>(&row.at(field));
    if (value == nullptr) {
        invalid_relationship(database, "Relationship catalog field has an invalid type or is NULL");
    }
    return *value;
}

RelationshipInfo make_relationship(const std::string& name, const Table& from,
                                    const Table& to, std::uint32_t flags) {
    auto join = RelationshipJoinType::inner;
    if ((flags & 0x01000000) != 0) {
        join = RelationshipJoinType::left_outer;
    } else if ((flags & 0x02000000) != 0) {
        join = RelationshipJoinType::right_outer;
    }
    return {name, from.name(), to.name(), {}, flags,
            (flags & 0x01) != 0, (flags & 0x02) == 0, (flags & 0x0100) != 0,
            (flags & 0x1000) != 0, (flags & 0x2000) != 0, join};
}

}

RelationshipBuilder::RelationshipBuilder(std::string name, std::string from_table, std::string to_table)
    : name_(std::move(name)),
      from_table_(std::move(from_table)),
      to_table_(std::move(to_table)) {}

RelationshipBuilder& RelationshipBuilder::add_columns(std::string from_column, std::string to_column) {
    columns_.push_back({std::move(from_column), std::move(to_column)});
    return *this;
}

RelationshipBuilder& RelationshipBuilder::with_referential_integrity(bool enabled) {
    referential_integrity_ = enabled;
    return *this;
}

RelationshipBuilder& RelationshipBuilder::with_cascade_updates(bool enabled) {
    cascade_updates_ = enabled;
    return *this;
}

RelationshipBuilder& RelationshipBuilder::with_cascade_deletes(bool enabled) {
    cascade_deletes_ = enabled;
    return *this;
}

RelationshipBuilder& RelationshipBuilder::with_cascade_null_on_delete(bool enabled) {
    cascade_null_on_delete_ = enabled;
    return *this;
}

RelationshipBuilder& RelationshipBuilder::with_join_type(RelationshipJoinType join_type) {
    join_type_ = join_type;
    return *this;
}

const std::string& RelationshipBuilder::name() const noexcept { return name_; }
const std::string& RelationshipBuilder::from_table() const noexcept { return from_table_; }
const std::string& RelationshipBuilder::to_table() const noexcept { return to_table_; }
const std::vector<RelationshipColumnInfo>& RelationshipBuilder::columns() const noexcept { return columns_; }
bool RelationshipBuilder::referential_integrity() const noexcept { return referential_integrity_; }
bool RelationshipBuilder::cascade_updates() const noexcept { return cascade_updates_; }
bool RelationshipBuilder::cascade_deletes() const noexcept { return cascade_deletes_; }
bool RelationshipBuilder::cascade_null_on_delete() const noexcept { return cascade_null_on_delete_; }
RelationshipJoinType RelationshipBuilder::join_type() const noexcept { return join_type_; }

std::vector<RelationshipInfo> Database::relationships(bool include_system_tables) const {
    const auto local_tables = tables();
    std::map<std::string, const Table*> tables_by_name;
    const Table* catalog = nullptr;
    for (const auto& table : local_tables) {
        auto key = identifier_key(table.name());
        if (key == "msysrelationships" && !table.is_user_table()) {
            catalog = &table;
        }
        if (!table.is_linked() && (include_system_tables || table.is_user_table())) {
            tables_by_name.emplace(std::move(key), &table);
        }
    }
    if (catalog == nullptr) {
        for (const auto& diagnostic : diagnostics()) {
            if (diagnostic.table_name && identifier_key(*diagnostic.table_name) == "msysrelationships") {
                invalid_relationship(*this, "Relationship catalog schema could not be read");
            }
        }
        return {};
    }

    const auto name_field = field_index(*this, *catalog, "szRelationship");
    const auto from_table_field = field_index(*this, *catalog, "szReferencedObject");
    const auto to_table_field = field_index(*this, *catalog, "szObject");
    const auto from_column_field = field_index(*this, *catalog, "szReferencedColumn");
    const auto to_column_field = field_index(*this, *catalog, "szColumn");
    const auto count_field = field_index(*this, *catalog, "ccolumn");
    const auto ordinal_field = field_index(*this, *catalog, "icolumn");
    const auto flags_field = field_index(*this, *catalog, "grbit");

    struct PendingRelationship {
        RelationshipInfo info;
        std::int32_t count;
        std::map<std::int32_t, RelationshipColumnInfo> pairs;
    };
    std::vector<PendingRelationship> pending;
    std::map<std::string, std::size_t> by_name;
    auto rows = catalog->rows();
    while (const auto row = rows.next()) {
        const auto& from_name = relationship_field<std::string>(*this, *row, from_table_field);
        const auto& to_name = relationship_field<std::string>(*this, *row, to_table_field);
        const auto from = tables_by_name.find(identifier_key(from_name));
        const auto to = tables_by_name.find(identifier_key(to_name));
        // Follow Jackcess enumeration for missing endpoints and the system-table filter.
        if (from == tables_by_name.end() || to == tables_by_name.end()) {
            continue;
        }
        const auto& from_table = *from->second;
        const auto& to_table = *to->second;
        const auto count = relationship_field<std::int32_t>(*this, *row, count_field);
        const auto ordinal = relationship_field<std::int32_t>(*this, *row, ordinal_field);
        if (count <= 0 || ordinal < 0 || ordinal >= count) {
            invalid_relationship(*this, "Relationship column count or ordinal is invalid");
        }
        const auto& name = relationship_field<std::string>(*this, *row, name_field);
        const auto flags = static_cast<std::uint32_t>(relationship_field<std::int32_t>(*this, *row, flags_field));
        const auto position = by_name.emplace(identifier_key(name), pending.size());
        if (position.second) {
            pending.push_back({make_relationship(name, from_table, to_table, flags), count, {}});
        }
        auto& group = pending[position.first->second];
        auto& relationship = group.info;
        if (relationship.from_table != from_table.name() || relationship.to_table != to_table.name() ||
            relationship.raw_flags != flags || group.count != count) {
            invalid_relationship(*this, "Relationship catalog rows disagree on their definition");
        }
        const auto& from_name_field = relationship_field<std::string>(*this, *row, from_column_field);
        const auto& to_name_field = relationship_field<std::string>(*this, *row, to_column_field);
        const auto& from_column = from_table.columns()[field_index(*this, from_table, from_name_field)];
        const auto& to_column = to_table.columns()[field_index(*this, to_table, to_name_field)];
        if (!group.pairs.emplace(ordinal, RelationshipColumnInfo{from_column.name, to_column.name}).second) {
            invalid_relationship(*this, "Relationship column ordinal is duplicated");
        }
    }
    std::vector<RelationshipInfo> result;
    result.reserve(pending.size());
    for (auto& group : pending) {
        if (group.pairs.size() != static_cast<std::size_t>(group.count)) {
            invalid_relationship(*this, "Relationship is missing a column mapping");
        }
        group.info.columns.reserve(group.pairs.size());
        for (auto& pair : group.pairs) {
            group.info.columns.push_back(std::move(pair.second));
        }
        result.push_back(std::move(group.info));
    }
    return result;
}

std::string detail::prepare_relationship_name(const Database& database,
                                               const RelationshipBuilder& relationship) {
    constexpr std::size_t maximum_name_length = 64;
    auto base = relationship.name();
    if (!std::all_of(base.begin(), base.end(), [](unsigned char c) { return c < 0x80; })) {
        throw DatabaseError(DatabaseErrorCode::unsupported_operation,
                            "Relationship names must use ASCII characters",
                            database.path());
    }
    if (base.size() > maximum_name_length) {
        throw std::invalid_argument("Relationship name exceeds the 64-character limit");
    }
    if (!base.empty() &&
        (base.front() == ' ' ||
         std::all_of(base.begin(), base.end(), [](unsigned char c) { return c == ' '; }) ||
         std::any_of(base.begin(), base.end(), [](unsigned char c) {
             return c < 0x20 || c == '.' || c == '!' || c == '`' || c == ']' || c == '[';
         }))) {
        throw std::invalid_argument("Relationship name is blank or contains invalid identifier characters");
    }

    const auto existing = database.relationships();
    const auto parent = identifier_key(relationship.from_table());
    const auto child = identifier_key(relationship.to_table());
    std::set<std::string> names;
    for (const auto& item : existing) {
        names.insert(identifier_key(item.name));
    }
    const auto tables = database.tables();
    const auto endpoint = [&](const std::string& key) {
        return std::find_if(tables.begin(), tables.end(), [&](const Table& table) {
            return table.is_user_table() && identifier_key(table.name()) == key;
        });
    };
    const auto parent_table = endpoint(parent);
    const auto child_table = endpoint(child);
    if (base.empty() && parent_table != tables.end() && child_table != tables.end()) {
        base = parent_table->name().substr(0, maximum_name_length / 2 - 3) + child_table->name();
    }
    base = base.substr(0, maximum_name_length - 3);
    if (relationship.referential_integrity() && child_table != tables.end()) {
        for (const auto& index : child_table->indexes()) {
            names.insert(identifier_key(index.name));
        }
    }
    std::string candidate = base;
    for (std::size_t suffix = 1; names.count(identifier_key(candidate)) != 0; ++suffix) {
        const auto digits = std::to_string(suffix);
        candidate = base.substr(0, maximum_name_length - digits.size()) + digits;
    }

    if (!relationship.referential_integrity() ||
        !(relationship.cascade_updates() || relationship.cascade_deletes() ||
          relationship.cascade_null_on_delete())) {
        return candidate;
    }

    std::map<std::string, std::vector<std::string>> edges;
    for (const auto& item : existing) {
        if (item.cascade_updates || item.cascade_deletes) {
            edges[identifier_key(item.from_table)].push_back(identifier_key(item.to_table));
        }
    }
    std::vector<std::string> pending{child};
    std::set<std::string> visited;
    while (!pending.empty()) {
        auto current = std::move(pending.back());
        pending.pop_back();
        if (current == parent) {
            throw std::invalid_argument("Relationship would create a cascading relationship cycle");
        }
        if (visited.insert(current).second) {
            const auto next = edges.find(current);
            if (next != edges.end()) {
                pending.insert(pending.end(), next->second.begin(), next->second.end());
            }
        }
    }
    return candidate;
}

Table Database::open_linked_table(std::string_view name, const LinkedTableResolver& resolver) const {
    if (!resolver) {
        throw std::invalid_argument("A resolver is required to open a linked table");
    }
    auto table = open_linked_table(name, resolver, {});
    if (!table) {
        throw DatabaseError(DatabaseErrorCode::unsupported_operation,
                            "Linked table name was not found", path());
    }
    return std::move(*table);
}

std::optional<Table> Database::open_linked_table(std::string_view name,
                                                 const LinkedTableResolver& file_resolver,
                                                 const CustomLinkedSourceResolver& custom_resolver) const {
    if (!file_resolver && !custom_resolver) {
        throw std::invalid_argument("A file or custom resolver is required to open a linked table");
    }

    const auto resolve_database = [&](const Database& source, const LinkedTableInfo& link)
        -> std::optional<Database> {
        const auto cache_state = [&](const std::string& key,
                                     const std::function<std::shared_ptr<detail::DatabaseState>()>& open) {
            {
                std::lock_guard<std::mutex> lock(source.state_->linked_databases_mutex);
                const auto cached = source.state_->linked_databases.find(key);
                if (cached != source.state_->linked_databases.end()) {
                    return cached->second;
                }
            }

            auto candidate = open();
            std::lock_guard<std::mutex> lock(source.state_->linked_databases_mutex);
            const auto [cached, inserted] = source.state_->linked_databases.emplace(key, candidate);
            (void)inserted;
            return cached->second;
        };

        if (custom_resolver) {
            const auto source_key = custom_source_cache_key(link);
            std::shared_ptr<CustomLinkedSource> custom_source;
            bool source_resolution_cached = false;
            {
                std::lock_guard<std::mutex> lock(source.state_->linked_databases_mutex);
                const auto cached = source.state_->custom_linked_sources.find(source_key);
                if (cached != source.state_->custom_linked_sources.end()) {
                    custom_source = cached->second;
                    source_resolution_cached = true;
                }
            }

            if (!source_resolution_cached) {
                auto candidate = custom_resolver(source.path(), link);
                std::lock_guard<std::mutex> lock(source.state_->linked_databases_mutex);
                const auto [cached, inserted] =
                    source.state_->custom_linked_sources.emplace(source_key, std::move(candidate));
                (void)inserted;
                custom_source = cached->second;
            }

            if (custom_source) {
                const auto key = custom_table_cache_key(source_key, *link.target_table);
                {
                    std::lock_guard<std::mutex> lock(source.state_->linked_databases_mutex);
                    const auto cached = source.state_->linked_databases.find(key);
                    if (cached != source.state_->linked_databases.end()) {
                        return Database(cached->second);
                    }
                }
                auto snapshot = custom_source->load_table(*link.target_table);
                if (!snapshot) {
                    return std::nullopt;
                }
                auto linked_state = cache_state(key, [&] {
                    auto materialized = materialize_custom_linked_table(source.path(), link,
                                                                         std::move(*snapshot),
                                                                         source.state_->writable
                                                                             ? DatabaseOpenMode::read_write
                                                                             : DatabaseOpenMode::read_only);
                    materialized.first.state_->temporary_directory = std::move(materialized.second);
                    materialized.first.state_->custom_source = custom_source;
                    return materialized.first.state_;
                });
                return Database(std::move(linked_state));
            }
        }

        if (link.kind != LinkedTableKind::file || !link.target_database || link.target_database->empty()) {
            throw DatabaseError(DatabaseErrorCode::unsupported_operation,
                                "The custom resolver did not provide this non-file linked table",
                                source.path());
        }
        if (!file_resolver) {
            throw DatabaseError(DatabaseErrorCode::unsupported_operation,
                                "The custom resolver declined the linked table and no file resolver is available",
                                source.path());
        }
        const auto& linkee_name = *link.target_database;
        auto linked_state = cache_state(linkee_name, [&] {
            const auto target = file_resolver(source.path(), link);
            if (target.empty()) {
                throw DatabaseError(DatabaseErrorCode::unsupported_operation,
                                    "The resolver declined the linked table", source.path());
            }
            auto opened_database = Database::open(target, source.state_->writable
                                                              ? DatabaseOpenMode::read_write
                                                              : DatabaseOpenMode::read_only);
            return opened_database.state_;
        });
        return Database(std::move(linked_state));
    };

    const Database* current = this;
    std::optional<Database> opened;
    std::string requested(name);
    std::set<std::pair<std::filesystem::path, std::string>> visited;
    constexpr std::size_t maximum_link_depth = 32;
    for (std::size_t depth = 0; depth < maximum_link_depth; ++depth) {
        const auto key = identifier_key(requested);
        auto links = current->linked_tables();
        const auto link = std::find_if(links.begin(), links.end(), [&](const auto& item) {
            return identifier_key(item.name) == key;
        });
        if (link == links.end()) {
            return std::nullopt;
        }
        if (!link->target_table || link->target_table->empty() ||
            (link->kind == LinkedTableKind::file &&
             (!link->target_database || link->target_database->empty()))) {
            throw DatabaseError(DatabaseErrorCode::unsupported_operation,
                                "The linked table has no supported target identifier", current->path());
        }
        std::error_code error;
        const auto canonical = std::filesystem::weakly_canonical(current->path(), error);
        if (!visited.emplace(error ? current->path().lexically_normal() : canonical, key).second) {
            throw DatabaseError(DatabaseErrorCode::unsupported_operation,
                                "Linked table resolution forms a cycle", current->path());
        }

        requested = *link->target_table;
        auto resolved_database = resolve_database(*current, *link);
        if (!resolved_database) {
            return std::nullopt;
        }
        auto database = std::move(*resolved_database);
        auto tables = database.tables();
        const auto target_key = identifier_key(requested);
        const auto table = std::find_if(tables.begin(), tables.end(), [&](const auto& item) {
            return identifier_key(item.name()) == target_key;
        });
        if (table != tables.end()) {
            return *table;
        }
        opened = std::move(database);
        current = &*opened;
    }

    throw DatabaseError(DatabaseErrorCode::unsupported_operation,
                        "Linked table resolution exceeded the depth limit", path());
}

}
