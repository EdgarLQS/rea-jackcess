import io.github.spannm.jackcess.ColumnBuilder;
import io.github.spannm.jackcess.CursorBuilder;
import io.github.spannm.jackcess.DataType;
import io.github.spannm.jackcess.Database;
import io.github.spannm.jackcess.DatabaseBuilder;
import io.github.spannm.jackcess.Table;
import io.github.spannm.jackcess.TableBuilder;
import java.math.BigDecimal;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.security.MessageDigest;
import java.time.LocalDateTime;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.UUID;

/**
 * Creates the Jet4 read fixtures used by tests/jet4_data_read_test.cpp.
 *
 * <p>The existing Jet4 fixtures (fk-*, query-*) only carry table and index definitions, so every
 * user table is empty. This fixture adds rows that exercise the read path with the fixed Jackcess
 * 5.2.0 JAR as the writer:
 *
 * <ul>
 *   <li>row 1: every supported column type populated at a non-NULL value,
 *   <li>row 2: every nullable column NULL,
 *   <li>row 3: a MEMO value longer than one data page,
 *   <li>row 4: a short row updated into a longer MEMO, which migrates the row to an overflow row,
 *   <li>row 5: numeric and date boundaries, including empty TEXT/MEMO and a pre-epoch date,
 *   <li>row 6: text that stresses JSON escaping and UTF-16 surrogate pairs.
 * </ul>
 *
 * <p>Regenerate both fixtures after changing this file; the committed SHA-256 values live in
 * tests/fixtures/README.md. The dump printed on stdout is the fixed-JAR reading of the same file,
 * so it can be compared with the C++ canonical output without a comparer.
 */
public final class JackcessJet4DataFixture {

    private static final String TABLE = "DataRows";

    private static final int LONG_MEMO_LENGTH = 9000;

    private static final UUID GUID = UUID.fromString("00112233-4455-6677-8899-aabbccddeeff");

    private static final LocalDateTime DATE = LocalDateTime.of(2020, 1, 2, 3, 4, 5);

    private static final LocalDateTime PRE_EPOCH_DATE = LocalDateTime.of(1899, 12, 29, 18, 30, 0);

    private static byte[] ramp(int size, int start) {
        byte[] bytes = new byte[size];
        for (int index = 0; index < size; index++) {
            bytes[index] = (byte) ((start + index) & 0xff);
        }
        return bytes;
    }

    private static String longMemo() {
        StringBuilder builder = new StringBuilder(LONG_MEMO_LENGTH + 16);
        while (builder.length() < LONG_MEMO_LENGTH) {
            builder.append("0123456789abcdef");
        }
        return builder.append("-尾-𝄞").toString();
    }

    private static void createTable(Database db) throws Exception {
        TableBuilder builder = new TableBuilder(TABLE)
            .addColumn(new ColumnBuilder("Id", DataType.LONG))
            .addColumn(new ColumnBuilder("Flag", DataType.BOOLEAN))
            .addColumn(new ColumnBuilder("ByteVal", DataType.BYTE))
            .addColumn(new ColumnBuilder("IntVal", DataType.INT))
            .addColumn(new ColumnBuilder("LongVal", DataType.LONG))
            .addColumn(new ColumnBuilder("FloatVal", DataType.FLOAT))
            .addColumn(new ColumnBuilder("DoubleVal", DataType.DOUBLE))
            .addColumn(new ColumnBuilder("MoneyVal", DataType.MONEY))
            .addColumn(new ColumnBuilder("DateVal", DataType.SHORT_DATE_TIME))
            .addColumn(new ColumnBuilder("GuidVal", DataType.GUID))
            .addColumn(new ColumnBuilder("TextVal", DataType.TEXT).withLengthInUnits(50))
            .addColumn(new ColumnBuilder("BinVal", DataType.BINARY).withLength(32))
            .addColumn(new ColumnBuilder("MemoVal", DataType.MEMO))
            .addColumn(new ColumnBuilder("OleVal", DataType.OLE))
            .withPrimaryKey("Id");
        builder.toTable(db);
    }

    private static void insertRows(Database db) throws Exception {
        Table table = db.getTable(TABLE);

        Map<String, Object> row = new LinkedHashMap<>();
        row.put("Id", 1);
        row.put("Flag", Boolean.TRUE);
        row.put("ByteVal", (byte) 127);
        row.put("IntVal", (short) 32767);
        row.put("LongVal", Integer.MAX_VALUE);
        row.put("FloatVal", 1.5f);
        row.put("DoubleVal", -2.25d);
        row.put("MoneyVal", new BigDecimal("1234.5678"));
        row.put("DateVal", DATE);
        row.put("GuidVal", GUID);
        row.put("TextVal", "中文 Ünïcode ✓ 𝄞");
        row.put("BinVal", ramp(16, 0));
        row.put("MemoVal", "短备忘录");
        row.put("OleVal", ramp(24, 0x80));
        table.addRowFromMap(row);

        row = new LinkedHashMap<>();
        row.put("Id", 2);
        table.addRowFromMap(row);

        row = new LinkedHashMap<>();
        row.put("Id", 3);
        row.put("MemoVal", longMemo());
        table.addRowFromMap(row);

        row = new LinkedHashMap<>();
        row.put("Id", 4);
        row.put("MemoVal", "seed");
        table.addRowFromMap(row);

        row = new LinkedHashMap<>();
        row.put("Id", 5);
        row.put("Flag", Boolean.FALSE);
        row.put("ByteVal", (byte) -128);
        row.put("IntVal", (short) -32768);
        row.put("LongVal", Integer.MIN_VALUE);
        row.put("FloatVal", -0.5f);
        row.put("DoubleVal", Double.MAX_VALUE);
        row.put("MoneyVal", new BigDecimal("-0.0001"));
        row.put("DateVal", PRE_EPOCH_DATE);
        row.put("GuidVal", new UUID(0L, 0L));
        row.put("TextVal", "");
        row.put("BinVal", new byte[0]);
        row.put("MemoVal", "");
        row.put("OleVal", new byte[0]);
        table.addRowFromMap(row);

        row = new LinkedHashMap<>();
        row.put("Id", 6);
        row.put("TextVal", "quote\" backslash\\ tab\t newline\n cr\r end");
        row.put("MemoVal", "第一行\r\n第二行 𝄞");
        table.addRowFromMap(row);

        // Growing a stored MEMO past one page makes Jackcess migrate the row to an overflow row.
        var cursor = CursorBuilder.createCursor(table);
        if (!cursor.findFirstRow(Map.of("Id", 4))) {
            throw new IllegalStateException("row 4 is missing");
        }
        Map<String, Object> updated = new LinkedHashMap<>(cursor.getCurrentRow());
        updated.put("MemoVal", longMemo());
        cursor.updateCurrentRowFromMap(updated);
        cursor.reset();
    }

    private static String hex(byte[] bytes) {
        StringBuilder builder = new StringBuilder(bytes.length * 2);
        for (byte value : bytes) {
            builder.append(String.format("%02x", value));
        }
        return builder.toString();
    }

    private static String sha256(byte[] bytes) throws Exception {
        return hex(MessageDigest.getInstance("SHA-256").digest(bytes));
    }

    private static String quote(Object value) {
        if (value == null) {
            return "null";
        }
        if (value instanceof byte[]) {
            byte[] bytes = (byte[]) value;
            return "blob(" + bytes.length + ")";
        }
        return value.toString().replace("\r", "\\r").replace("\n", "\\n").replace("\t", "\\t");
    }

    private static void dump(Path path) throws Exception {
        List<String> lines = new ArrayList<>();
        try (Database db = DatabaseBuilder.open(path.toFile())) {
            Table table = db.getTable(TABLE);
            lines.add("table " + TABLE);
            lines.add("columns " + table.getColumns().stream()
                .map(column -> column.getName() + ":" + column.getType())
                .reduce((left, right) -> left + "," + right).orElse(""));
            for (Map<String, Object> row : table) {
                StringBuilder builder = new StringBuilder("row");
                for (var column : table.getColumns()) {
                    builder.append('\t').append(quote(row.get(column.getName())));
                }
                lines.add(builder.toString());
            }
        }
        lines.add("sha256 " + sha256(Files.readAllBytes(path)));
        System.out.println(String.join("\n", lines));
    }

    public static void main(String[] args) throws Exception {
        Path path = Path.of(args[0]);
        String format = args.length > 1 ? args[1] : "V2000";
        try (Database db = new DatabaseBuilder().withPath(path)
                .withFileFormat(Database.FileFormat.valueOf(format)).create()) {
            createTable(db);
            insertRows(db);
        }
        dump(path);
    }
}
