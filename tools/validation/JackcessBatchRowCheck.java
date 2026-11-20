import io.github.spannm.jackcess.Database;
import io.github.spannm.jackcess.DatabaseBuilder;
import io.github.spannm.jackcess.Row;
import io.github.spannm.jackcess.Table;

import java.nio.file.Paths;
import java.util.Iterator;

/** Verifies C++ batch insertion and a nullable value with Jackcess 5.2.0. */
public final class JackcessBatchRowCheck {
    private JackcessBatchRowCheck() {}

    public static void main(String[] args) throws Exception {
        if (args.length != 1) {
            throw new IllegalArgumentException("Usage: JackcessBatchRowCheck <database.mdb>");
        }

        try (Database database = new DatabaseBuilder().withPath(Paths.get(args[0])).withReadOnly(true).open()) {
            Table table = database.getTable("T");
            if (table == null || table.getColumns().size() != 2) {
                throw new AssertionError("Jackcess could not find the expected two-column table T");
            }

            Iterator<Row> rows = table.iterator();
            for (int value = 0; value < 500; ++value) {
                if (!rows.hasNext()) {
                    throw new AssertionError("Jackcess reached end of table before row " + value);
                }
                Row row = rows.next();
                if (!Integer.valueOf(value).equals(row.get("Id")) ||
                    !Integer.valueOf(value + 1000).equals(row.get("GroupId"))) {
                    throw new AssertionError("C++ batch row values differ at input row " + value);
                }
            }

            if (!rows.hasNext()) {
                throw new AssertionError("Jackcess did not find the final nullable row");
            }
            Row nullableRow = rows.next();
            if (!Integer.valueOf(501).equals(nullableRow.get("Id")) || nullableRow.get("GroupId") != null) {
                throw new AssertionError("Jackcess did not preserve the final NULL value");
            }
            if (rows.hasNext()) {
                throw new AssertionError("Jackcess found rows beyond the expected batch");
            }
        }

        System.out.println("Jackcess 5.2.0 verified 500 batch rows and the nullable row");
    }
}
