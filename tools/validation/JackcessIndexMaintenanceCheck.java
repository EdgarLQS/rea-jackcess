import io.github.spannm.jackcess.Cursor;
import io.github.spannm.jackcess.CursorBuilder;
import io.github.spannm.jackcess.Database;
import io.github.spannm.jackcess.DatabaseBuilder;
import io.github.spannm.jackcess.Row;
import io.github.spannm.jackcess.Table;

import java.nio.file.Paths;
/**
 * Verifies that Jackcess 5.2.0 reads the index entries left by a C++ update and delete.
 *
 * The C++ fixture inserts Code 30/10/20, moves Code 30 to 25 and deletes the row with Code 10,
 * so the maintained index must expose exactly (20, 3) and (25, 1) in that order.
 */
public final class JackcessIndexMaintenanceCheck {
    private JackcessIndexMaintenanceCheck() {}

    public static void main(String[] args) throws Exception {
        if (args.length != 3) {
            throw new IllegalArgumentException(
                "Usage: JackcessIndexMaintenanceCheck <database.mdb> <table> <index>");
        }

        try (Database database = new DatabaseBuilder().withPath(Paths.get(args[0])).withReadOnly(true).open()) {
            Table table = database.getTable(args[1]);
            if (table == null || table.getRowCount() != 2) {
                throw new AssertionError("Jackcess did not find the expected two-row table");
            }
            Cursor cursor = new CursorBuilder(table).withIndexByName(args[2]).toCursor();
            int[][] expected = {{20, 3}, {25, 1}};
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

            for (Row row : table) {
                if (((Number) row.get("Id")).intValue() == 2) {
                    throw new AssertionError("Jackcess still observed the deleted row");
                }
            }
        }

        System.out.println("Jackcess 5.2.0 read the updated and deleted index entries correctly");
    }
}
