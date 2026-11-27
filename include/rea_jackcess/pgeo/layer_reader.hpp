#pragma once

#include "rea_jackcess/database.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace rea_jackcess::pgeo {

// Spatial reference system metadata for a layer, read from GDB_SpatialRefs.
struct SpatialReferenceInfo {
    // SRID recorded in the table.
    std::int32_t id;
    // Preserved as stored in GDB_SpatialRefs.SRTEXT; it may be WKT, a GUID, or another form.
    std::optional<std::string> definition;
};

// One PGeo Feature Class (layer) exposed by a database.
struct LayerInfo {
    // User-facing layer name.
    std::string name;
    // Physical table that backs the layer.
    std::string table_name;
    // Column holding the OBJECTID.
    std::string object_id_column;
    // Column holding the raw SHAPE geometry blob.
    std::string shape_column;
    // ESRI shape type code of the geometry (e.g. 1 = point, 5 = polygon).
    std::int32_t shape_type;
    // True when the geometry carries Z values.
    bool has_z;
    // True when the geometry carries M (measure) values.
    bool has_m;
    // SRID of the layer's spatial reference, when recorded.
    std::optional<std::int32_t> spatial_reference_id;
    // Resolved spatial reference metadata, when its row exists.
    std::optional<SpatialReferenceInfo> spatial_reference;
    // The layer's non-spatial attribute columns.
    std::vector<ColumnInfo> attributes;
};

// One attribute (column value) of a feature.
struct Attribute {
    // Attribute column name.
    std::string name;
    // Decoded value.
    Value value;
};

// One feature: its OBJECTID, its attribute values, and its raw geometry bytes.
struct Feature {
    // The feature's OBJECTID.
    std::int32_t object_id;
    // Attribute values in the layer's attribute-column order.
    std::vector<Attribute> attributes;
    // nullopt is a NULL SHAPE; an engaged empty vector is a zero-length SHAPE value.
    std::optional<std::vector<std::uint8_t>> shape;
};

// Iterates the features of one layer. Move-only; the underlying dataset handle outlives it.
class LayerReader {
public:
    LayerReader(LayerReader&&) noexcept;
    LayerReader& operator=(LayerReader&&) noexcept;
    ~LayerReader();

    LayerReader(const LayerReader&) = delete;
    LayerReader& operator=(const LayerReader&) = delete;

    // Returns the next feature, or nullopt once the layer is exhausted (or on a moved-from reader).
    std::optional<Feature> next();

private:
    struct Impl;
    explicit LayerReader(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;

    friend class Dataset;
};

// An open PGeo-enabled database: its layer list and a factory for per-layer readers.
class Dataset {
public:
    // Opens the database read-only and discovers its Feature Classes.
    // @param path database file to open.
    // @throws DatabaseError on I/O or format failure.
    static Dataset open(const std::filesystem::path& path);

    // All layers discovered in the dataset.
    const std::vector<LayerInfo>& layers() const noexcept;
    // Opens a feature reader for the named layer.
    // @param name layer name (as reported in layers()).
    // @throws DatabaseError (unsupported_operation) when no layer has that name.
    LayerReader open_layer(std::string_view name) const;

private:
    Dataset(Database database, std::vector<LayerInfo> layers);

    // Owns the underlying database handle.
    Database database_;
    // Discovered layers.
    std::vector<LayerInfo> layers_;
};

}
