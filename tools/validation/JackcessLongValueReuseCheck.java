import io.github.spannm.jackcess.Database;
import io.github.spannm.jackcess.DatabaseBuilder;
import io.github.spannm.jackcess.Row;
import io.github.spannm.jackcess.Table;

import java.nio.file.Paths;
import java.util.Iterator;

/** Verifies that Jackcess 5.2.0 reads a MEMO value that reused an existing LVAL page. */
public final class JackcessLongValueReuseCheck {
    private JackcessLongValueReuseCheck() {}

    public static void main(String[] args) throws Exception {
        if (args.length != 1) {
            throw new IllegalArgumentException("Usage: JackcessLongValueReuseCheck <database.mdb>");
        }

        try (Database database = new DatabaseBuilder().withPath(Paths.get(args[0])).withReadOnly(true).open()) {
            Table table = database.getTable("Reuse");
            if (table == null || table.getColumns().size() != 3 || table.getRowCount() != 1) {
                throw new AssertionError("Jackcess could not find the expected single-row reuse table");
            }

            Iterator<Row> rows = table.iterator();
            Row row = rows.next();
            if (((Number) row.get("Id")).intValue() != 1 || ((Number) row.get("Tail")).intValue() != 7) {
                throw new AssertionError("Jackcess observed unexpected fixed columns in the reuse table");
            }
            Object memo = row.get("Memo");
            if (!(memo instanceof String) || ((String) memo).length() != 300
                    || !((String) memo).chars().allMatch(value -> value == 'E')) {
                throw new AssertionError("Jackcess did not read the reused MEMO value");
            }
            if (rows.hasNext()) {
                throw new AssertionError("Jackcess observed an unexpected extra row in the reuse table");
            }
        }

        System.out.println("Jackcess 5.2.0 read a MEMO value stored on a reused LVAL page");
    }
}
