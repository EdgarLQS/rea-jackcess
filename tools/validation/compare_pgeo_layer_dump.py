#!/usr/bin/env python3
"""Compare PGeo feature classes in C++ canonical and Jackcess table dumps by OBJECTID.

Usage: compare_pgeo_layer_dump.py <cpp-canonical.json> <jackcess-table-dump.json>
       [--source <database.mdb>]
"""

import argparse
import hashlib
import json
import sys

sys.dont_write_bytecode = True

from compare_table_dump import cell_equal


def _column_names(table, source):
    if not isinstance(table, dict):
        raise ValueError(f"{source} dump contains a malformed table record")
    columns = table.get("columns")
    if not isinstance(columns, list):
        raise ValueError(f"{source} table {table.get('name')} has no column list")
    names = []
    for column in columns:
        if not isinstance(column, dict):
            raise ValueError(f"{source} table {table.get('name')} has a malformed column")
        name = column.get("name")
        if not isinstance(name, str):
            raise ValueError(f"{source} table {table.get('name')} has an invalid column name")
        names.append(name)
    folded = [name.casefold() for name in names]
    if len(folded) != len(set(folded)):
        raise ValueError(f"{source} table {table.get('name')} has duplicate column names")
    return names


def _tables(document, source):
    if not isinstance(document, dict):
        raise ValueError(f"{source} dump is not a JSON object")
    if document.get("errors"):
        raise ValueError(f"{source} dump contains canonical errors")
    table_records = document.get("tables")
    if not isinstance(table_records, list):
        raise ValueError(f"{source} dump has no table list")
    result = {}
    for table in table_records:
        if not isinstance(table, dict):
            raise ValueError(f"{source} dump contains a malformed table record")
        name = table.get("name")
        if not isinstance(name, str):
            raise ValueError(f"{source} dump contains a table without a name")
        key = name.casefold()
        if key in result:
            raise ValueError(f"{source} dump contains duplicate table {name}")
        _column_names(table, source)
        result[key] = table
    return result


def _required_table(tables, name, source):
    table = tables.get(name.casefold())
    if table is None:
        raise ValueError(f"{source} dump is missing {name}")
    return table


def _row_dicts(table, source):
    names = _column_names(table, source)
    rows = table.get("rows")
    if not isinstance(rows, list):
        raise ValueError(f"{source} table {table['name']} has no row list")
    for row_number, row in enumerate(rows, start=1):
        if not isinstance(row, list) or len(row) != len(names):
            raise ValueError(f"{source} table {table['name']} row {row_number} has the wrong width")
        yield dict(zip(names, row))


def _get(mapping, name, context):
    for key, value in mapping.items():
        if key.casefold() == name.casefold():
            return value
    raise ValueError(f"{context} is missing column {name}")


def _guid_key(value, context):
    if isinstance(value, dict):
        value = value.get("$guid")
    if not isinstance(value, str):
        raise ValueError(f"{context} is not a GUID")
    compact = value.strip("{}").replace("-", "").casefold()
    if len(compact) != 32 or any(char not in "0123456789abcdef" for char in compact):
        raise ValueError(f"{context} is not a valid GUID")
    return compact


def _text(value, context):
    if not isinstance(value, str) or not value:
        raise ValueError(f"{context} is not non-empty text")
    return value


def _column_schema(table, source):
    result = []
    for column in table["columns"]:
        if source == "cpp":
            kind = column.get("mdbType")
            size = column.get("size")
        else:
            kind = column.get("type")
            size = column.get("length")
        if (not isinstance(kind, str) or not isinstance(size, int)
                or isinstance(size, bool)):
            raise ValueError(f"{source} table {table['name']} has incomplete column metadata")
        if kind == "SHORT":
            kind = "INT"
        result.append((column["name"], kind, size))
    return result


def _extract_layers(document, source):
    tables = _tables(document, source)
    item_types = _required_table(tables, "GDB_ItemTypes", source)
    items = _required_table(tables, "GDB_Items", source)
    geometry = _required_table(tables, "GDB_GeomColumns", source)
    item_types_rows = list(_row_dicts(item_types, source))
    feature_types = {
        _guid_key(_get(row, "UUID", f"{source} GDB_ItemTypes"),
                  f"{source} GDB_ItemTypes.UUID")
        for row in item_types_rows
        if isinstance(_get(row, "Name", f"{source} GDB_ItemTypes"), str)
        and _get(row, "Name", f"{source} GDB_ItemTypes").casefold() == "feature class"
    }
    if len(feature_types) != 1:
        raise ValueError(f"{source} dump has {len(feature_types)} Feature Class type IDs")

    geometry_rows = list(_row_dicts(geometry, source))
    spatial_refs = tables.get("gdb_spatialrefs")
    spatial_by_id = {}
    if spatial_refs is not None:
        for row in _row_dicts(spatial_refs, source):
            srid = _get(row, "SRID", f"{source} GDB_SpatialRefs")
            if srid is not None:
                if srid in spatial_by_id:
                    raise ValueError(f"{source} GDB_SpatialRefs has duplicate SRID {srid}")
                spatial_by_id[srid] = _get(row, "SRTEXT", f"{source} GDB_SpatialRefs")

    layers = {}
    for item in _row_dicts(items, source):
        if _guid_key(_get(item, "Type", f"{source} GDB_Items"),
                     f"{source} GDB_Items.Type") not in feature_types:
            continue
        name = _text(_get(item, "Name", f"{source} GDB_Items"), f"{source} layer name")
        table_name = _text(_get(item, "PhysicalName", f"{source} GDB_Items"),
                           f"{source} layer {name} physical table")
        shape_name = _text(_get(item, "DatasetInfo1", f"{source} GDB_Items"),
                           f"{source} layer {name} shape field")
        key = name.casefold()
        if key in layers:
            raise ValueError(f"{source} dump contains duplicate PGeo layer {name}")

        matches = [
            row for row in geometry_rows
            if isinstance(_get(row, "TableName", f"{source} GDB_GeomColumns"), str)
            and isinstance(_get(row, "FieldName", f"{source} GDB_GeomColumns"), str)
            and _get(row, "TableName", f"{source} GDB_GeomColumns").casefold()
            == table_name.casefold()
            and _get(row, "FieldName", f"{source} GDB_GeomColumns").casefold()
            == shape_name.casefold()
        ]
        if len(matches) != 1:
            raise ValueError(
                f"{source} layer {name} resolves to {len(matches)} GDB_GeomColumns rows"
            )
        geometry_row = matches[0]
        layer_table = tables.get(table_name.casefold())
        if layer_table is None:
            raise ValueError(f"{source} layer {name} table {table_name} is missing")
        schema = _column_schema(layer_table, source)
        object_ids = [column for column in schema if column[0].casefold() == "objectid"]
        shapes = [column for column in schema if column[0].casefold() == shape_name.casefold()]
        if len(object_ids) != 1 or len(shapes) != 1:
            raise ValueError(f"{source} layer {name} must have one OBJECTID and one {shape_name}")
        if object_ids[0][1] != "LONG":
            raise ValueError(f"{source} layer {name} OBJECTID is {object_ids[0][1]}, not LONG")
        if shapes[0][1] not in ("OLE", "BINARY", "BIG_BINARY"):
            raise ValueError(f"{source} layer {name} {shape_name} is not a BLOB column")
        attributes = [
            column for column in schema
            if column[0].casefold() not in {"objectid", shape_name.casefold()}
        ]

        srid = _get(geometry_row, "SRID", f"{source} GDB_GeomColumns")
        layers[key] = {
            "name": name,
            "table_name": table_name,
            "shape_name": shape_name,
            "shape_type": _get(geometry_row, "ShapeType", f"{source} GDB_GeomColumns"),
            "srid": srid,
            "has_z": _get(geometry_row, "HasZ", f"{source} GDB_GeomColumns"),
            "has_m": _get(geometry_row, "HasM", f"{source} GDB_GeomColumns"),
            "srs_text": spatial_by_id.get(srid),
            "schema": schema,
            "attributes": attributes,
            "table": layer_table,
            "object_id_index": [c[0] for c in schema].index(object_ids[0][0]),
            "shape_index": [c[0] for c in schema].index(shapes[0][0]),
        }
    if not layers:
        raise ValueError(f"{source} dump contains no PGeo Feature Classes")
    return layers


def _features_by_objectid(layer, source):
    result = {}
    rows = layer["table"].get("rows")
    if not isinstance(rows, list):
        raise ValueError(f"{source} table {layer['table']['name']} has no row list")
    for row_number, row in enumerate(rows, start=1):
        if not isinstance(row, list) or len(row) != len(layer["schema"]):
            raise ValueError(
                f"{source} layer {layer['name']} row {row_number} has the wrong width"
            )
        object_id = row[layer["object_id_index"]]
        if isinstance(object_id, bool) or not isinstance(object_id, int):
            raise ValueError(
                f"{source} layer {layer['name']} row {row_number} has a non-integer OBJECTID"
            )
        if object_id in result:
            raise ValueError(
                f"{source} layer {layer['name']} has duplicate OBJECTID {object_id}"
            )
        result[object_id] = row
    return result


def compare_documents(cpp_document, java_document):
    """Return (differences, [(layer name, matched features, attribute count), ...])."""
    problems = []
    try:
        cpp_layers = _extract_layers(cpp_document, "cpp")
        java_layers = _extract_layers(java_document, "Jackcess")
    except (KeyError, TypeError, ValueError) as error:
        return [str(error)], []

    for key in sorted(cpp_layers.keys() - java_layers.keys()):
        problems.append(f"{cpp_layers[key]['name']}: only in the C++ PGeo catalog")
    for key in sorted(java_layers.keys() - cpp_layers.keys()):
        problems.append(f"{java_layers[key]['name']}: only in the Jackcess PGeo catalog")

    layer_summaries = []
    for key in sorted(cpp_layers.keys() & java_layers.keys()):
        cpp_layer = cpp_layers[key]
        java_layer = java_layers[key]
        name = cpp_layer["name"]
        for field in ("name", "table_name", "shape_name"):
            if cpp_layer[field] != java_layer[field]:
                problems.append(
                    f"{name}: {field} {cpp_layer[field]!r} != {java_layer[field]!r}"
                )
        for field in ("shape_type", "srid", "has_z", "has_m", "srs_text"):
            if not cell_equal(cpp_layer[field], java_layer[field]):
                problems.append(f"{name}: {field} differs")
        if cpp_layer["schema"] != java_layer["schema"]:
            problems.append(f"{name}: feature table column definitions differ")
        if cpp_layer["attributes"] != java_layer["attributes"]:
            problems.append(f"{name}: attribute definitions differ")

        try:
            cpp_features = _features_by_objectid(cpp_layer, "cpp")
            java_features = _features_by_objectid(java_layer, "Jackcess")
        except ValueError as error:
            problems.append(str(error))
            continue
        cpp_ids = set(cpp_features)
        java_ids = set(java_features)
        for object_id in sorted(cpp_ids - java_ids):
            problems.append(f"{name}: OBJECTID {object_id} only in the C++ table")
        for object_id in sorted(java_ids - cpp_ids):
            problems.append(f"{name}: OBJECTID {object_id} only in the Jackcess table")

        id_column_names = {"objectid", cpp_layer["shape_name"].casefold()}
        cpp_attribute_indices = [
            index for index, column in enumerate(cpp_layer["schema"])
            if column[0].casefold() not in id_column_names
        ]
        java_attribute_indices = [
            index for index, column in enumerate(java_layer["schema"])
            if column[0].casefold() not in id_column_names
        ]
        common_ids = sorted(cpp_ids & java_ids)
        for object_id in common_ids:
            cpp_row = cpp_features[object_id]
            java_row = java_features[object_id]
            for column, cpp_index, java_index in zip(
                cpp_layer["attributes"], cpp_attribute_indices, java_attribute_indices
            ):
                if not cell_equal(cpp_row[cpp_index], java_row[java_index]):
                    problems.append(
                        f"{name} OBJECTID {object_id} attribute {column[0]} differs"
                    )
            cpp_shape = cpp_row[cpp_layer["shape_index"]]
            java_shape = java_row[java_layer["shape_index"]]
            if not cell_equal(cpp_shape, java_shape):
                problems.append(
                    f"{name} OBJECTID {object_id} SHAPE length/SHA-256 differs"
                )
        layer_summaries.append((name, len(common_ids), len(cpp_layer["attributes"])))
    return problems, layer_summaries


def _sha256(path):
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for block in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("cpp_dump")
    parser.add_argument("jackcess_dump")
    parser.add_argument("--source", help="also verify and report the unchanged database SHA-256")
    args = parser.parse_args(argv)

    try:
        before_hash = _sha256(args.source) if args.source else None
        with open(args.cpp_dump, encoding="utf-8") as handle:
            cpp_document = json.load(handle)
        with open(args.jackcess_dump, encoding="utf-8") as handle:
            java_document = json.load(handle)
    except (OSError, json.JSONDecodeError) as error:
        print(f"Could not read comparison input: {error}", file=sys.stderr)
        return 2

    problems, layer_summaries = compare_documents(cpp_document, java_document)
    after_hash = None
    if args.source:
        try:
            after_hash = _sha256(args.source)
        except OSError as error:
            print(f"Could not recheck source SHA-256: {error}", file=sys.stderr)
            return 2
        if before_hash != after_hash:
            problems.append(
                f"source SHA-256 changed: {before_hash} -> {after_hash}"
            )
    if problems:
        print(f"{len(problems)} PGeo difference(s):")
        for problem in problems:
            print(f"  {problem}")
        return 1

    feature_count = sum(features for _, features, _ in layer_summaries)
    print(f"Compared {len(layer_summaries)} PGeo layers and "
          f"{feature_count} features by OBJECTID.")
    for name, features, attributes in layer_summaries:
        print(f"  {name}: {features} features, {attributes} attributes")
    print("Layer catalogs, attribute definitions and values, and SHAPE lengths/SHA-256 match.")
    if args.source:
        print(f"Source SHA-256 unchanged: {before_hash}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
