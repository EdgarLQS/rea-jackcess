import io.github.spannm.jackcess.ColumnBuilder;
import io.github.spannm.jackcess.DataType;
import io.github.spannm.jackcess.Database;
import io.github.spannm.jackcess.DatabaseBuilder;
import io.github.spannm.jackcess.Table;
import io.github.spannm.jackcess.TableBuilder;
import java.math.BigDecimal;
import java.nio.file.Files;
import java.nio.file.Path;
import java.security.MessageDigest;
import java.time.LocalDateTime;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.UUID;

/**
 * Creates the ACE V2016 and extra V2019 read fixtures used by tests/ace_extra_read_test.cpp.
 *
 * <p>The committed ACE fixtures only cover V2007, V2010, and the single-table V2019 extended-date
 * resource, so V2016 has no sample at all. These files are written by the fixed Jackcess 5.2.0 JAR
 * and read back by the C++ library:
 *
 * <ul>
 *   <li>V2016: an ordinary table with the scalar types plus TEXT/MEMO/OLE, NULL rows, non-ASCII
 *       text, and a cross-page MEMO,
 *   <li>V2019: a second sample with an EXT_DATE_TIME column covering NULL, nanosecond precision,
 *       and the years 100, 1265, and 1765, plus an ordinary TEXT column.
 * </ul>
 */
public final class JackcessAceFixture {

    private static final LocalDateTime STAMP = LocalDateTime.of(2021, 6, 7, 8, 9, 10);

    private static String longMemo() {
        StringBuilder builder = new StringBuilder(5000);
        while (builder.length() < 5000) {
            builder.append("ace-memo-0123456789");
        }
        return builder.append("-尾").toString();
    }

    private static void createV2016(Database db) throws Exception {
        new TableBuilder("V2016Data")
            .addColumn(new ColumnBuilder("Id", DataType.LONG))
            .addColumn(new ColumnBuilder("Flag", DataType.BOOLEAN))
            .addColumn(new ColumnBuilder("Name", DataType.TEXT).withLengthInUnits(30))
            .addColumn(new ColumnBuilder("Amount", DataType.MONEY))
            .addColumn(new ColumnBuilder("Created", DataType.SHORT_DATE_TIME))
            .addColumn(new ColumnBuilder("Ref", DataType.GUID))
            .addColumn(new ColumnBuilder("Memo", DataType.MEMO))
            .addColumn(new ColumnBuilder("Payload", DataType.OLE))
            .withPrimaryKey("Id")
            .toTable(db);

        Table table = db.getTable("V2016Data");
        Map<String, Object> row = new LinkedHashMap<>();
        row.put("Id", 1);
        row.put("Flag", Boolean.TRUE);
        row.put("Name", "V2016 中文 ✓");
        row.put("Amount", new BigDecimal("9876.5432"));
        row.put("Created", STAMP);
        row.put("Ref", UUID.fromString("aabbccdd-1122-3344-5566-77889900aabb"));
        row.put("Memo", longMemo());
        row.put("Payload", new byte[] {1, 2, 3, 4, 5});
        table.addRowFromMap(row);

        row = new LinkedHashMap<>();
        row.put("Id", 2);
        table.addRowFromMap(row);

        row = new LinkedHashMap<>();
        row.put("Id", 3);
        row.put("Name", "");
        row.put("Memo", "");
        row.put("Payload", new byte[0]);
        table.addRowFromMap(row);
    }

    private static void createV2019(Database db) throws Exception {
        new TableBuilder("V2019Data")
            .addColumn(new ColumnBuilder("Id", DataType.LONG))
            .addColumn(new ColumnBuilder("Stamp", DataType.EXT_DATE_TIME))
            .addColumn(new ColumnBuilder("Note", DataType.TEXT).withLengthInUnits(40))
            .withPrimaryKey("Id")
            .toTable(db);

        Table table = db.getTable("V2019Data");
        addStamp(table, 1, null, "null");
        addStamp(table, 2, LocalDateTime.of(2020, 1, 2, 3, 4, 5, 123456789), "nanos");
        addStamp(table, 3, LocalDateTime.of(100, 3, 4, 5, 6, 7), "year-100");
        addStamp(table, 4, LocalDateTime.of(1265, 7, 8, 9, 10, 11), "year-1265");
        addStamp(table, 5, LocalDateTime.of(1765, 12, 13, 14, 15, 16), "year-1765");
        addStamp(table, 6, LocalDateTime.of(1899, 12, 29, 18, 30, 0), "pre-epoch");
    }

    private static void addStamp(Table table, int id, LocalDateTime stamp, String note) throws Exception {
        Map<String, Object> row = new LinkedHashMap<>();
        row.put("Id", id);
        row.put("Stamp", stamp);
        row.put("Note", note);
        table.addRowFromMap(row);
    }

    private static String hex(byte[] bytes) {
        StringBuilder builder = new StringBuilder(bytes.length * 2);
        for (byte value : bytes) {
            builder.append(String.format("%02x", value));
        }
        return builder.toString();
    }

    private static void dump(Path path, String tableName) throws Exception {
        List<String> lines = new ArrayList<>();
        try (Database db = DatabaseBuilder.open(path.toFile())) {
            Table table = db.getTable(tableName);
            lines.add("table " + tableName);
            for (var column : table.getColumns()) {
                lines.add("  column " + column.getName() + ":" + column.getType()
                    + ":len=" + column.getLength());
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
        lines.add("sha256 " + hex(MessageDigest.getInstance("SHA-256").digest(Files.readAllBytes(path))));
        System.out.println(String.join("\n", lines));
    }

    public static void main(String[] args) throws Exception {
        Path path = Path.of(args[0]);
        String format = args.length > 1 ? args[1] : "V2016";
        try (Database db = new DatabaseBuilder().withPath(path)
                .withFileFormat(Database.FileFormat.valueOf(format)).create()) {
            if (format.equals("V2019")) {
                createV2019(db);
            } else {
                createV2016(db);
            }
        }
        dump(path, format.equals("V2019") ? "V2019Data" : "V2016Data");
    }
}
