import io.github.spannm.jackcess.ColumnBuilder;
import io.github.spannm.jackcess.DataType;
import io.github.spannm.jackcess.Database;
import io.github.spannm.jackcess.DatabaseBuilder;
import io.github.spannm.jackcess.IndexBuilder;
import io.github.spannm.jackcess.Table;
import io.github.spannm.jackcess.TableBuilder;
import java.nio.file.Files;
import java.nio.file.Path;
import java.security.MessageDigest;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

/**
 * Creates the Jet4 composite-index fixtures used by tests/jet4_index_read_test.cpp.
 *
 * <p>{@code RegionCode} carries a unique composite TEXT key over {@code (Region, Code)} with
 * ignore-nulls, so the fixture covers three read situations the single-column fixture cannot:
 *
 * <ul>
 *   <li>rows whose key is fully or partly NULL stay out of the index scan but remain visible to
 *       the sequential scan,
 *   <li>a prefix-equal key pair such as {@code A} and {@code AA} fixes the ordering inside a
 *       shared leading column,
 *   <li>prefix, range, reverse and exact-match scans over two TEXT columns.
 * </ul>
 *
 * <p>{@code LongValues} stores a 9000-byte OLE payload that spans several data pages.
 */
public final class JackcessJet4IndexFixture {

    private static final int OLE_LENGTH = 9000;

    private static byte[] ramp(int size, int start) {
        byte[] bytes = new byte[size];
        for (int index = 0; index < size; index++) {
            bytes[index] = (byte) ((start + index) & 0xff);
        }
        return bytes;
    }

    private static void addKeyRow(Table table, int id, String region, String code) throws Exception {
        Map<String, Object> row = new LinkedHashMap<>();
        row.put("Id", id);
        row.put("Region", region);
        row.put("Code", code);
        row.put("Seq", id * 10);
        row.put("Note", "note-" + id);
        table.addRowFromMap(row);
    }

    private static void createTables(Database db) throws Exception {
        new TableBuilder("RegionCode")
            .addColumn(new ColumnBuilder("Id", DataType.LONG))
            .addColumn(new ColumnBuilder("Region", DataType.TEXT).withLengthInUnits(8))
            .addColumn(new ColumnBuilder("Code", DataType.TEXT).withLengthInUnits(8))
            .addColumn(new ColumnBuilder("Seq", DataType.LONG))
            .addColumn(new ColumnBuilder("Note", DataType.MEMO))
            .withPrimaryKey("Id")
            .addIndex(new IndexBuilder("RegionCodeKey")
                .withColumns("Region", "Code")
                .withUnique()
                .withIgnoreNulls())
            .toTable(db);

        new TableBuilder("LongValues")
            .addColumn(new ColumnBuilder("Id", DataType.LONG))
            .addColumn(new ColumnBuilder("OleVal", DataType.OLE))
            .addColumn(new ColumnBuilder("MemoVal", DataType.MEMO))
            .addColumn(new ColumnBuilder("BinMax", DataType.BINARY).withLength(255))
            .withPrimaryKey("Id")
            .toTable(db);

        // A MEMO key is truncated to 255 UTF-16 units for indexing, and BINARY stops at 255 bytes.
        new TableBuilder("MemoKeys")
            .addColumn(new ColumnBuilder("Id", DataType.LONG))
            .addColumn(new ColumnBuilder("MemoKey", DataType.MEMO))
            .addColumn(new ColumnBuilder("BinMax", DataType.BINARY).withLength(255))
            .withPrimaryKey("Id")
            .addIndex(new IndexBuilder("MemoKeyIdx").withColumns("MemoKey"))
            .toTable(db);
    }

    private static void addMemoRow(Table table, int id, String memo, byte[] binary) throws Exception {
        Map<String, Object> row = new LinkedHashMap<>();
        row.put("Id", id);
        row.put("MemoKey", memo);
        row.put("BinMax", binary);
        table.addRowFromMap(row);
    }

    private static void insertRows(Database db) throws Exception {
        Table table = db.getTable("RegionCode");
        // Insertion order deliberately differs from key order so an index scan cannot pass by
        // accident on physical order.
        addKeyRow(table, 1, "EU", "A");
        addKeyRow(table, 2, "EU", "B");
        addKeyRow(table, 3, "US", "A");
        addKeyRow(table, 4, "AS", "A");
        addKeyRow(table, 5, "EU", "C");
        addKeyRow(table, 6, null, "Z");
        addKeyRow(table, 7, "EU", null);
        // A fully NULL key would collide with row 6 in the unique index (Jackcess stores both as
        // a null entry and warns about the duplicate), so the fixture keeps one NULL per position.
        addKeyRow(table, 9, "US", "B");
        // "AA" sorts after "A": a shorter key that is a prefix sorts first.
        addKeyRow(table, 10, "EU", "AA");

        Map<String, Object> row = new LinkedHashMap<>();
        row.put("Id", 1);
        row.put("OleVal", ramp(OLE_LENGTH, 0x40));
        row.put("MemoVal", "ole-page");
        row.put("BinMax", ramp(255, 0x10));
        db.getTable("LongValues").addRowFromMap(row);

        Table memos = db.getTable("MemoKeys");
        addMemoRow(memos, 1, "memo-b", ramp(255, 0x20));
        addMemoRow(memos, 2, "memo-a", ramp(255, 0x30));
        addMemoRow(memos, 3, "memo-c", ramp(255, 0x40));
        addMemoRow(memos, 4, "memo-d" + "x".repeat(600), ramp(255, 0x50));
        addMemoRow(memos, 5, null, new byte[0]);
    }

    private static String hex(byte[] bytes) {
        StringBuilder builder = new StringBuilder(bytes.length * 2);
        for (byte value : bytes) {
            builder.append(String.format("%02x", value));
        }
        return builder.toString();
    }

    private static void dump(Path path) throws Exception {
        List<String> lines = new ArrayList<>();
        try (Database db = DatabaseBuilder.open(path.toFile())) {
            for (String name : List.of("RegionCode", "LongValues", "MemoKeys")) {
                Table table = db.getTable(name);
                lines.add("table " + name);
                for (var column : table.getColumns()) {
                    lines.add("  column " + column.getName() + ":" + column.getType()
                        + ":len=" + column.getLength());
                }
                for (var index : table.getIndexes()) {
                    StringBuilder builder = new StringBuilder("  index " + index.getName()
                        + " unique=" + index.isUnique() + " ignoreNulls=" + index.shouldIgnoreNulls()
                        + " columns=");
                    for (var column : index.getColumns()) {
                        builder.append(column.getName()).append(column.isAscending() ? "+" : "-").append(',');
                    }
                    lines.add(builder.toString());
                }
                for (Map<String, Object> row : table) {
                    StringBuilder builder = new StringBuilder("  row");
                    for (var column : table.getColumns()) {
                        Object value = row.get(column.getName());
                        builder.append('\t').append(value instanceof byte[] bytes
                            ? "blob(" + bytes.length + ")" : String.valueOf(value));
                    }
                    lines.add(builder.toString());
                }
            }
        }
        byte[] bytes = Files.readAllBytes(path);
        lines.add("sha256 " + hex(MessageDigest.getInstance("SHA-256").digest(bytes)));
        System.out.println(String.join("\n", lines));
    }

    public static void main(String[] args) throws Exception {
        Path path = Path.of(args[0]);
        String format = args.length > 1 ? args[1] : "V2000";
        try (Database db = new DatabaseBuilder().withPath(path)
                .withFileFormat(Database.FileFormat.valueOf(format)).create()) {
            createTables(db);
            insertRows(db);
        }
        dump(path);
    }
}
