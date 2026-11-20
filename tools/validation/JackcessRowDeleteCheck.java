import io.github.spannm.jackcess.Database;
import io.github.spannm.jackcess.DatabaseBuilder;
import io.github.spannm.jackcess.Row;
import io.github.spannm.jackcess.Table;

import java.nio.file.Paths;
import java.util.Iterator;

/** Verifies C++ row deletion with Jackcess 5.2.0. */
public final class JackcessRowDeleteCheck {
    private JackcessRowDeleteCheck() {}

    public static void main(String[] args) throws Exception {
        if (args.length != 1) {
            throw new IllegalArgumentException("Usage: JackcessRowDeleteCheck <database.mdb>");
        }

        try (Database database = new DatabaseBuilder().withPath(Paths.get(args[0])).withReadOnly(true).open()) {
            Table table = database.getTable("T");
            if (table == null || table.getColumns().size() != 2) {
                throw new AssertionError("Jackcess could not find the expected two-column table T");
            }
            Iterator<Row> rows = table.iterator();
            if (!rows.hasNext() || !Integer.valueOf(1).equals(rows.next().get("Id")) ||
                    !rows.hasNext() || !Integer.valueOf(3).equals(rows.next().get("Id")) || rows.hasNext()) {
                throw new AssertionError("Jackcess did not observe the expected rows after deletion");
            }
        }

        System.out.println("Jackcess 5.2.0 verified row deletion and remaining row order");
    }
}
