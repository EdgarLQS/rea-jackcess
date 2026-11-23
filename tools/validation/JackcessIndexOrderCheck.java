import io.github.spannm.jackcess.Cursor;
import io.github.spannm.jackcess.CursorBuilder;
import io.github.spannm.jackcess.Database;
import io.github.spannm.jackcess.DatabaseBuilder;
import io.github.spannm.jackcess.Row;
import io.github.spannm.jackcess.Table;

import java.nio.file.Paths;

/** Verifies that Jackcess 5.2.0 reads index entries maintained by the C++ row writer. */
public final class JackcessIndexOrderCheck {
    private JackcessIndexOrderCheck() {}

    public static void main(String[] args) throws Exception {
        if (args.length != 3) {
            throw new IllegalArgumentException(
                "Usage: JackcessIndexOrderCheck <database.mdb> <table> <index>");
        }

        try (Database database = new DatabaseBuilder().withPath(Paths.get(args[0])).withReadOnly(true).open()) {
            Table table = database.getTable(args[1]);
            if (table == null || table.getRowCount() != 3) {
                throw new AssertionError("Jackcess did not find the expected three-row table");
            }
            Cursor cursor = new CursorBuilder(table).withIndexByName(args[2]).toCursor();
            int[][] expected = {{10, 2}, {20, 3}, {30, 1}};
            for (int[] entry : expected) {
                Row row = cursor.getNextRow();
                if (row == null
                        || ((Number) row.get("Code")).intValue() != entry[0]
                        || ((Number) row.get("Id")).intValue() != entry[1]) {
                    throw new AssertionError("Jackcess did not read index entry " + entry[0] + ":" + entry[1]);
                }
            }
            if (cursor.getNextRow() != null) {
                throw new AssertionError("Jackcess observed an unexpected extra index entry");
            }
        }

        System.out.println("Jackcess 5.2.0 read the maintained index entries in ascending key order");
    }
}
