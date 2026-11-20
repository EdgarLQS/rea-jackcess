import io.github.spannm.jackcess.Database;
import io.github.spannm.jackcess.DatabaseBuilder;
import io.github.spannm.jackcess.Row;
import io.github.spannm.jackcess.Table;

import java.nio.file.Paths;
import java.util.Iterator;

/** Verifies C++ fixed-width row updates with Jackcess 5.2.0. */
public final class JackcessRowUpdateCheck {
    private JackcessRowUpdateCheck() {}

    public static void main(String[] args) throws Exception {
        if (args.length != 1) {
            throw new IllegalArgumentException("Usage: JackcessRowUpdateCheck <database.mdb>");
        }

        try (Database database = new DatabaseBuilder().withPath(Paths.get(args[0])).withReadOnly(true).open()) {
            Table table = database.getTable("T");
            if (table == null || table.getColumns().size() != 2) {
                throw new AssertionError("Jackcess could not find the expected two-column table T");
            }
            Iterator<Row> rows = table.iterator();
            if (!rows.hasNext()) {
                throw new AssertionError("Jackcess did not find the updated row");
            }
            Row row = rows.next();
            if (!Integer.valueOf(11).equals(row.get("Id")) || row.get("GroupId") != null || rows.hasNext()) {
                throw new AssertionError("Jackcess did not observe the in-place update");
            }
        }

        System.out.println("Jackcess 5.2.0 verified the updated LONG row and NULL value");
    }
}
