import io.github.spannm.jackcess.Database;
import io.github.spannm.jackcess.DatabaseBuilder;
import io.github.spannm.jackcess.Table;

import java.nio.file.Paths;
import java.util.Iterator;

/** Writes a row into a C++-created table with the pinned Jackcess 5.2.0 reference. */
public final class JackcessRowWriteProbe {
    private JackcessRowWriteProbe() {}

    public static void main(String[] args) throws Exception {
        if (args.length != 1) {
            throw new IllegalArgumentException("Usage: JackcessRowWriteProbe <database.mdb>");
        }

        try (Database database = new DatabaseBuilder().withPath(Paths.get(args[0])).open()) {
            Table table = database.getTable("T");
            if (table == null || !"Id".equals(table.getColumns().get(0).getName())) {
                throw new AssertionError("Jackcess could not find the expected C++-created table schema");
            }
            if (table.iterator().hasNext()) {
                throw new AssertionError("Expected the C++-created table to be empty before the probe");
            }
            table.addRow(42);
            Iterator<?> rows = table.iterator();
            if (!rows.hasNext()) {
                throw new AssertionError("Jackcess did not persist the inserted row");
            }
            System.out.println("Jackcess 5.2.0 wrote one row into T");
        }

        try (Database database = new DatabaseBuilder().withPath(Paths.get(args[0])).withReadOnly(true).open()) {
            Table table = database.getTable("T");
            if (table == null || !table.iterator().hasNext()) {
                throw new AssertionError("Jackcess could not reopen the row it wrote");
            }
        }
    }
}
