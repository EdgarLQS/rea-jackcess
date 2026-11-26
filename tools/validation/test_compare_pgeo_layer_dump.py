import json
import sys
import tempfile
import unittest
from contextlib import redirect_stdout
from io import StringIO
from pathlib import Path

sys.dont_write_bytecode = True

from compare_pgeo_layer_dump import main


FEATURE_CLASS_GUID = "{70737809-852c-4a03-9e22-2cecea5b9bfa}"
SHAPE_ONE = {"$blob": {"size": 3, "sha256": "0123456789abcdef"}}


def table(name, columns, rows, cpp):
    if cpp:
        column_records = [
            {"name": column, "mdbType": kind, "size": size}
            for column, kind, size in columns
        ]
    else:
        column_records = [
            {"name": column, "type": kind, "length": size}
            for column, kind, size in columns
        ]
    return {"name": name, "columns": column_records, "rows": rows}


def document(cpp, feature_rows):
    guid = {"$guid": FEATURE_CLASS_GUID} if cpp else FEATURE_CLASS_GUID.upper()
    return {
        "errors": [] if cpp else None,
        "tables": [
            table("GDB_ItemTypes", [("UUID", "GUID", 16), ("Name", "TEXT", 510)],
                  [[guid, "Feature Class"]], cpp),
            table("GDB_Items", [("Type", "GUID", 16), ("Name", "TEXT", 510),
                                ("PhysicalName", "TEXT", 510), ("DatasetInfo1", "TEXT", 510)],
                  [[guid, "sample", "Features", "SHAPE"]], cpp),
            table("GDB_GeomColumns", [("TableName", "TEXT", 510), ("FieldName", "TEXT", 510),
                                      ("ShapeType", "LONG", 4), ("SRID", "LONG", 4),
                                      ("HasZ", "BOOLEAN", 1), ("HasM", "BOOLEAN", 1)],
                  [["Features", "SHAPE", 1, 4326, False, False]], cpp),
            table("GDB_SpatialRefs", [("SRID", "LONG", 4), ("SRTEXT", "MEMO", 0)],
                  [[4326, "EPSG:4326"]], cpp),
            table("Features", [("OBJECTID", "LONG", 4), ("SHAPE", "OLE", 0),
                               ("Label", "TEXT", 510)], feature_rows, cpp),
        ],
    }


def run_comparator(cpp_document, java_document):
    with tempfile.TemporaryDirectory() as directory:
        cpp_path = Path(directory) / "cpp.json"
        java_path = Path(directory) / "java.json"
        cpp_path.write_text(json.dumps(cpp_document), encoding="utf-8")
        java_path.write_text(json.dumps(java_document), encoding="utf-8")
        output = StringIO()
        with redirect_stdout(output):
            exit_code = main([str(cpp_path), str(java_path)])
    return exit_code, output.getvalue()


class PGeoLayerDumpComparisonTest(unittest.TestCase):
    def setUp(self):
        self.cpp = document(True, [[1, SHAPE_ONE, "one"], [2, None, None]])
        self.java = document(False, [[2, None, None], [1, SHAPE_ONE, "one"]])

    def test_matches_features_by_objectid_and_reports_layer_counts(self):
        exit_code, output = run_comparator(self.cpp, self.java)

        self.assertEqual(exit_code, 0)
        self.assertIn("Compared 1 PGeo layers and 2 features by OBJECTID.", output)
        self.assertIn("sample: 2 features, 1 attributes", output)

    def test_reports_attribute_difference_with_objectid(self):
        self.java["tables"][-1]["rows"][1][2] = "changed"

        exit_code, output = run_comparator(self.cpp, self.java)

        self.assertEqual(exit_code, 1)
        self.assertIn("sample OBJECTID 1 attribute Label differs", output)

    def test_reports_shape_hash_difference_with_objectid(self):
        self.java["tables"][-1]["rows"][1][1] = {
            "$blob": {"size": 3, "sha256": "fedcba9876543210"}
        }

        exit_code, output = run_comparator(self.cpp, self.java)

        self.assertEqual(exit_code, 1)
        self.assertIn("sample OBJECTID 1 SHAPE length/SHA-256 differs", output)

    def test_rejects_duplicate_objectids(self):
        self.cpp["tables"][-1]["rows"].append([1, SHAPE_ONE, "duplicate"])

        exit_code, output = run_comparator(self.cpp, self.java)

        self.assertEqual(exit_code, 1)
        self.assertIn("duplicate OBJECTID 1", output)

    def test_rejects_missing_feature_rows_instead_of_treating_them_as_empty(self):
        self.cpp["tables"][-1]["rows"] = None

        exit_code, output = run_comparator(self.cpp, self.java)

        self.assertEqual(exit_code, 1)
        self.assertIn("cpp table Features has no row list", output)


if __name__ == "__main__":
    unittest.main()
