import io.github.spannm.jackcess.Column;
import io.github.spannm.jackcess.ColumnBuilder;
import io.github.spannm.jackcess.Cursor;
import io.github.spannm.jackcess.CursorBuilder;
import io.github.spannm.jackcess.DataType;
import io.github.spannm.jackcess.Database;
import io.github.spannm.jackcess.DatabaseBuilder;
import io.github.spannm.jackcess.Index;
import io.github.spannm.jackcess.IndexBuilder;
import io.github.spannm.jackcess.Row;
import io.github.spannm.jackcess.RowId;
import io.github.spannm.jackcess.Table;
import io.github.spannm.jackcess.TableBuilder;
import io.github.spannm.jackcess.impl.CursorImpl;
import io.github.spannm.jackcess.impl.TableImpl;

import java.io.File;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.util.HashSet;
import java.util.Iterator;
import java.util.Locale;
import java.util.Set;

/**
 * Manual Jackcess 5.2.0 reference probe for variable row growth, long values,
 * index-key updates, and deletion. Run only on a disposable database copy.
 */
public final class JackcessRowMigrationProbe {
    private static final String TABLE_NAME = "P0MigrationProbe";
    private static final String INDEX_NAME = "idxProbeKey";
    private static final int FILLER_ROWS = 40;
    private static final int TEXT_COLUMN_BYTES = 255;
    private static final int TEXT_COLUMN_CHARS = 127;
    private static final int LONG_MEMO_CHARS = 20_000;

    private JackcessRowMigrationProbe() {
    }

    public static void main(String[] args) throws Exception {
        if (args.length != 1) {
            throw new IllegalArgumentException("Usage: JackcessRowMigrationProbe <writable-database-copy>");
        }

        File file = new File(args[0]);
        int targetId = createProbeTable(file);
        System.out.println("format=" + fileFormat(file));
        System.out.println("prepared_rows=" + (FILLER_ROWS + 1));
        System.out.println("target_id=" + targetId);

        RowId keyUpdateBefore = updateKey(file, targetId);
        RowId keyUpdateAfter = readRowId(file, targetId);
        System.out.println("key_update_row_id_before=" + keyUpdateBefore);
        System.out.println("key_update_row_id_after=" + keyUpdateAfter);
        System.out.println("index_after_key_update=" + verifyIndex(file, targetId, FILLER_ROWS + 1));

        RowStateSnapshot migrationBefore = readRowState(file, targetId);
        updateVariableText(file, targetId);
        RowStateSnapshot migrationAfter = readRowState(file, targetId);
        if (!migrationAfter.overflow || migrationBefore.headerRowId.equals(migrationAfter.finalRowId)) {
            throw new IllegalStateException("Growing variable text did not produce an overflow row");
        }
        if (!migrationBefore.headerRowId.equals(migrationAfter.headerRowId)) {
            throw new IllegalStateException("Growing variable text changed the logical header RowId");
        }
        System.out.println("text_growth_overflow_before=" + migrationBefore.overflow);
        System.out.println("text_growth_overflow_after=" + migrationAfter.overflow);
        System.out.println("text_growth_header_row_id_before=" + migrationBefore.headerRowId);
        System.out.println("text_growth_header_row_id_after=" + migrationAfter.headerRowId);
        System.out.println("text_growth_overflow_row_id=" + migrationAfter.finalRowId);

        updateLongMemo(file, targetId);
        String memo = readMemo(file, targetId);
        if (memo.length() != LONG_MEMO_CHARS) {
            throw new IllegalStateException("Long memo update did not persist the requested character count");
        }
        System.out.println("long_memo_chars=" + memo.length());
        System.out.println("long_memo_sha256=" + sha256(memo));

        int beforeDelete = rowCount(file);
        deleteFillerTwice(file, 2);
        int afterDelete = rowCount(file);
        if (beforeDelete != FILLER_ROWS + 1 || afterDelete != beforeDelete - 1) {
            throw new IllegalStateException("Delete did not decrement the table row count exactly once");
        }
        System.out.println("rows_before_delete=" + beforeDelete);
        System.out.println("rows_after_delete_and_repeat=" + afterDelete);
        System.out.println("index_after_delete=" + verifyIndex(file, targetId, FILLER_ROWS));
    }

    private static int createProbeTable(File file) throws Exception {
        try (Database database = openReadWrite(file)) {
            if (database.getFileFormat() != Database.FileFormat.V2000) {
                throw new IllegalArgumentException("Probe requires a Jet4 V2000 database copy");
            }
            if (database.getTableNames().contains(TABLE_NAME)) {
                throw new IllegalArgumentException("Probe table already exists; use a fresh database copy");
            }

            Table table = new TableBuilder(TABLE_NAME)
                .addColumn(new ColumnBuilder("id", DataType.LONG).withAutoNumber(true))
                .addColumn(new ColumnBuilder("key", DataType.TEXT).withLength(32))
                .addColumn(new ColumnBuilder("body", DataType.TEXT).withLength(TEXT_COLUMN_BYTES))
                .addColumn(new ColumnBuilder("extra", DataType.TEXT).withLength(TEXT_COLUMN_BYTES))
                .addColumn(new ColumnBuilder("memo", DataType.MEMO))
                .withPrimaryKey("id")
                .addIndex(new IndexBuilder(INDEX_NAME).withColumns("key"))
                .toTable(database);

            Object[] target = table.addRow(Column.AUTO_NUMBER, "M_TARGET", "", "", "short memo");
            for (int index = 0; index < FILLER_ROWS; index++) {
                table.addRow(Column.AUTO_NUMBER,
                            String.format(Locale.ROOT, "F%02d", index),
                            repeat('F', TEXT_COLUMN_CHARS),
                            "",
                            "short memo");
            }
            return ((Number) target[0]).intValue();
        }
    }

    private static RowId updateKey(File file, int targetId) throws Exception {
        try (Database database = openReadWrite(file)) {
            Table table = database.getTable(TABLE_NAME);
            Row row = findRow(table, targetId);
            RowId before = row.getId();
            row.put("key", "Z_TARGET");
            table.updateRow(row);
            verifyIndex(table, targetId, FILLER_ROWS + 1);
            return before;
        }
    }

    private static void updateVariableText(File file, int targetId) throws Exception {
        try (Database database = openReadWrite(file)) {
            Table table = database.getTable(TABLE_NAME);
            Row row = findRow(table, targetId);
            row.put("body", repeat('B', TEXT_COLUMN_CHARS));
            row.put("extra", repeat('E', TEXT_COLUMN_CHARS));
            table.updateRow(row);
        }
    }

    private static void updateLongMemo(File file, int targetId) throws Exception {
        try (Database database = openReadWrite(file)) {
            Table table = database.getTable(TABLE_NAME);
            Row row = findRow(table, targetId);
            row.put("memo", repeat('M', LONG_MEMO_CHARS));
            table.updateRow(row);
        }
    }

    private static void deleteFillerTwice(File file, int fillerId) throws Exception {
        try (Database database = openReadWrite(file)) {
            Table table = database.getTable(TABLE_NAME);
            Row row = findRow(table, fillerId);
            table.deleteRow(row);
            table.deleteRow(row);
        }
    }

    private static Database.FileFormat fileFormat(File file) throws Exception {
        try (Database database = openReadOnly(file)) {
            return database.getFileFormat();
        }
    }

    private static RowId readRowId(File file, int id) throws Exception {
        try (Database database = openReadOnly(file)) {
            return findRow(database.getTable(TABLE_NAME), id).getId();
        }
    }

    private static RowStateSnapshot readRowState(File file, int id) throws Exception {
        try (Database database = openReadOnly(file)) {
            TableImpl table = (TableImpl) database.getTable(TABLE_NAME);
            Row row = findRow(table, id);
            CursorImpl cursor = table.getDefaultCursor();
            if (!cursor.findRow(row.getId())) {
                throw new IllegalStateException("RowId is not present in the default table cursor");
            }
            cursor.getCurrentRow();
            TableImpl.RowState rowState = cursor.getRowState();
            return new RowStateSnapshot(rowState.isOverflow(), rowState.getHeaderRowId(), rowState.getFinalRowId());
        }
    }

    private static String readMemo(File file, int id) throws Exception {
        try (Database database = openReadOnly(file)) {
            return (String) findRow(database.getTable(TABLE_NAME), id).get("memo");
        }
    }

    private static Row findRow(Table table, int id) throws Exception {
        Row row = CursorBuilder.findRowByPrimaryKey(table, id);
        if (row == null) {
            throw new IllegalStateException("Row not found for primary key " + id);
        }
        return row;
    }

    private static int rowCount(File file) throws Exception {
        try (Database database = openReadOnly(file)) {
            return database.getTable(TABLE_NAME).getRowCount();
        }
    }

    private static String verifyIndex(File file, int targetId, int expectedCount) throws Exception {
        try (Database database = openReadOnly(file)) {
            Table table = database.getTable(TABLE_NAME);
            return verifyIndex(table, targetId, expectedCount);
        }
    }

    private static String verifyIndex(Table table, int targetId, int expectedCount) throws Exception {
        Index index = table.getIndexes().stream()
            .filter(candidate -> INDEX_NAME.equals(candidate.getName()))
            .findFirst()
            .orElseThrow(() -> new IllegalStateException("Probe index was not found"));
        Cursor cursor = table.newCursor().withIndex(index).toCursor();
        Iterator<? extends Row> rows = cursor.iterator();
        Set<String> keys = new HashSet<>();
        String first = null;
        String last = null;
        while (rows.hasNext()) {
            String key = (String) rows.next().get("key");
            if (first == null) {
                first = key;
            }
            last = key;
            keys.add(key);
        }
        if (keys.size() != expectedCount || !keys.contains("Z_TARGET") || keys.contains("M_TARGET")) {
            throw new IllegalStateException("Index does not reflect the updated key or deleted row");
        }
        if (findRow(table, targetId).get("key").equals("Z_TARGET") && !"Z_TARGET".equals(last)) {
            throw new IllegalStateException("Updated key is not at the expected end of the index order");
        }
        return "entries=" + keys.size() + " first=" + first + " last=" + last;
    }

    private static Database openReadOnly(File file) throws Exception {
        return new DatabaseBuilder().withFile(file).withReadOnly(true).open();
    }

    private static Database openReadWrite(File file) throws Exception {
        return new DatabaseBuilder().withFile(file).withReadOnly(false).open();
    }

    private static String repeat(char value, int count) {
        char[] chars = new char[count];
        java.util.Arrays.fill(chars, value);
        return new String(chars);
    }

    private static String sha256(String value) throws Exception {
        byte[] digest = MessageDigest.getInstance("SHA-256").digest(value.getBytes(StandardCharsets.UTF_8));
        StringBuilder result = new StringBuilder(digest.length * 2);
        for (byte item : digest) {
            result.append(String.format(Locale.ROOT, "%02x", item & 0xff));
        }
        return result.toString();
    }

    private static final class RowStateSnapshot {
        private final boolean overflow;
        private final RowId headerRowId;
        private final RowId finalRowId;

        private RowStateSnapshot(boolean overflow, RowId headerRowId, RowId finalRowId) {
            this.overflow = overflow;
            this.headerRowId = headerRowId;
            this.finalRowId = finalRowId;
        }
    }
}
