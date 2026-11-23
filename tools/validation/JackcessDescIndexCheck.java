import io.github.spannm.jackcess.Cursor;
import io.github.spannm.jackcess.CursorBuilder;
import io.github.spannm.jackcess.Database;
import io.github.spannm.jackcess.DatabaseBuilder;
import io.github.spannm.jackcess.Row;
import io.github.spannm.jackcess.Table;

import java.nio.file.Paths;

/** Verifies that Jackcess 5.2.0 reads a descending index created by the C++ writer in 3,2,1 order. */
public final class JackcessDescIndexCheck {
    private JackcessDescIndexCheck() {}

    public static void main(String[] args) throws Exception {
        if (args.length != 1) {
            throw new IllegalArgumentException("Usage: JackcessDescIndexCheck <database.mdb>");
        }

        try (Database database = new DatabaseBuilder().withPath(Paths.get(args[0])).withReadOnly(true).open()) {
            Table table = database.getTable("Desc");
            if (table == null || table.getRowCount() != 3) {
                throw new AssertionError("Jackcess did not find the expected three-row Desc table");
            }
            Cursor cursor = new CursorBuilder(table).withIndexByName("IX_Id").toCursor();
            int[] expected = {3, 2, 1};
            for (int value : expected) {
                Row row = cursor.getNextRow();
                if (row == null || ((Number) row.get("Id")).intValue() != value) {
                    throw new AssertionError("Jackcess did not read descending key " + value);
                }
            }
            if (cursor.getNextRow() != null) {
                throw new AssertionError("Jackcess observed an unexpected extra descending index entry");
            }
        }

        System.out.println("Jackcess 5.2.0 read the descending index in 3,2,1 order");
    }
}
