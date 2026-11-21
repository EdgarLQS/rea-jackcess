import io.github.spannm.jackcess.Database;
import io.github.spannm.jackcess.DatabaseBuilder;
import io.github.spannm.jackcess.Row;
import io.github.spannm.jackcess.Table;

import java.nio.file.Paths;
import java.util.Iterator;

/** Writes and updates Jet4 BOOLEAN rows with the pinned Jackcess 5.2.0 reference. */
public final class JackcessBooleanRowWriteProbe {
    private JackcessBooleanRowWriteProbe() {}

    public static void main(String[] args) throws Exception {
        if (args.length != 1) {
            throw new IllegalArgumentException("Usage: JackcessBooleanRowWriteProbe <empty-boolean-database.mdb>");
        }

        try (Database database = new DatabaseBuilder().withPath(Paths.get(args[0])).open()) {
            Table table = database.getTable("Booleans");
            if (table == null || table.iterator().hasNext()) {
                throw new AssertionError("Expected an empty C++-created Booleans table");
            }

            table.addRow(Integer.valueOf(1), Boolean.TRUE, Boolean.FALSE, Integer.valueOf(10));
            table.addRow(Integer.valueOf(2), Boolean.FALSE, Boolean.TRUE, Integer.valueOf(-5));
            table.addRow(Integer.valueOf(3), null, null, Integer.valueOf(0));

            Iterator<Row> rows = table.iterator();
            Row updated = rows.next();
            updated.put("Flag", Boolean.FALSE);
            updated.put("Maybe", Boolean.TRUE);
            updated.put("Tail", Integer.valueOf(11));
            table.updateRow(updated);
        }

        try (Database database = new DatabaseBuilder().withPath(Paths.get(args[0])).withReadOnly(true).open()) {
            Table table = database.getTable("Booleans");
            boolean[][] expected = {
                {false, true},
                {false, true},
                {false, false},
            };
            if (table == null || table.getRowCount() != expected.length) {
                throw new AssertionError("Jackcess could not reopen the expected BOOLEAN rows");
            }

            Iterator<Row> rows = table.iterator();
            for (int index = 0; index < expected.length; index++) {
                if (!rows.hasNext()) {
                    throw new AssertionError("Jackcess observed fewer BOOLEAN rows than it wrote");
                }
                Row row = rows.next();
                if (!Boolean.valueOf(expected[index][0]).equals(row.get("Flag"))
                        || !Boolean.valueOf(expected[index][1]).equals(row.get("Maybe"))) {
                    throw new AssertionError("Jackcess did not preserve BOOLEAN bit values for row " + index);
                }
            }
            if (rows.hasNext()) {
                throw new AssertionError("Jackcess observed an unexpected extra BOOLEAN row");
            }
        }

        System.out.println("Jackcess 5.2.0 wrote, updated, and reopened Jet4 BOOLEAN values (NULL reads back as FALSE)");
    }
}
