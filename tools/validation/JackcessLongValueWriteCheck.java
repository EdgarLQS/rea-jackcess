import io.github.spannm.jackcess.Column;
import io.github.spannm.jackcess.DataType;
import io.github.spannm.jackcess.Database;
import io.github.spannm.jackcess.DatabaseBuilder;
import io.github.spannm.jackcess.Row;
import io.github.spannm.jackcess.Table;

import java.nio.file.Paths;
import java.util.Arrays;
import java.util.Iterator;
import java.util.List;

/** Verifies C++ Jet4 MEMO/OLE long-value writes with Jackcess 5.2.0. */
public final class JackcessLongValueWriteCheck {
    private JackcessLongValueWriteCheck() {}

    public static void main(String[] args) throws Exception {
        if (args.length != 1) {
            throw new IllegalArgumentException("Usage: JackcessLongValueWriteCheck <database.mdb>");
        }

        try (Database database = new DatabaseBuilder().withPath(Paths.get(args[0])).withReadOnly(true).open()) {
            Table table = database.getTable("LongValues");
            if (table == null || table.getRowCount() != 4 || table.getColumns().size() != 4) {
                throw new AssertionError("Jackcess could not find the expected four-row long-value table");
            }
            List<? extends Column> columns = table.getColumns();
            DataType[] expectedTypes = {DataType.LONG, DataType.MEMO, DataType.TEXT, DataType.OLE};
            String[] expectedNames = {"Id", "Memo", "Note", "Payload"};
            for (int index = 0; index < expectedTypes.length; ++index) {
                if (!expectedNames[index].equals(columns.get(index).getName()) ||
                    columns.get(index).getType() != expectedTypes[index]) {
                    throw new AssertionError("Jackcess observed the wrong MEMO/OLE schema");
                }
            }

            Iterator<Row> rows = table.iterator();
            Row updated = rows.next();
            byte[] expectedPayload = new byte[8192];
            Arrays.fill(expectedPayload, (byte) 0x6c);
            if (!Integer.valueOf(1).equals(updated.get("Id")) ||
                !"U".repeat(20000).equals(updated.get("Memo")) ||
                !"tail".equals(updated.get("Note")) ||
                !Arrays.equals(expectedPayload, (byte[]) updated.get("Payload"))) {
                throw new AssertionError("Jackcess did not read the updated multi-page MEMO/OLE values");
            }

            Row unicode = rows.next();
            if (!Integer.valueOf(2).equals(unicode.get("Id")) ||
                !"🔥雪".equals(unicode.get("Memo")) ||
                !"N".equals(unicode.get("Note")) ||
                !Arrays.equals(new byte[0], (byte[]) unicode.get("Payload"))) {
                throw new AssertionError("Jackcess confused Unicode Memo, empty OLE, or row offsets");
            }

            Row empty = rows.next();
            if (!Integer.valueOf(3).equals(empty.get("Id")) ||
                !"".equals(empty.get("Memo")) ||
                !"".equals(empty.get("Note")) ||
                !Arrays.equals(new byte[0], (byte[]) empty.get("Payload"))) {
                throw new AssertionError("Jackcess confused empty Memo/OLE values with NULL");
            }

            Row nulls = rows.next();
            if (!Integer.valueOf(4).equals(nulls.get("Id")) ||
                nulls.get("Memo") != null || nulls.get("Note") != null ||
                nulls.get("Payload") != null || rows.hasNext()) {
                throw new AssertionError("Jackcess did not preserve NULL MEMO/OLE values");
            }
        }

        System.out.println("Jackcess 5.2.0 verified multi-page MEMO/OLE writes, update, Unicode, empty, and NULL values");
    }
}
