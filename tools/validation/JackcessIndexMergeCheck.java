import io.github.spannm.jackcess.Cursor;
import io.github.spannm.jackcess.CursorBuilder;
import io.github.spannm.jackcess.Database;
import io.github.spannm.jackcess.DatabaseBuilder;
import io.github.spannm.jackcess.Row;
import io.github.spannm.jackcess.Table;

import java.nio.file.Paths;

/**
 * Verifies that Jackcess 5.2.0 reads an index whose lowest leaf C++ merged away.
 *
 * The fixture inserts 700 rows and deletes the first 400, which unlinks and releases the leaf
 * holding the smallest keys. Jackcess must see 300 rows, walk the shrunken leaf chain from 401 to 700 and
 * still accept new rows afterwards.
 */
public final class JackcessIndexMergeCheck {
    private JackcessIndexMergeCheck() {}

    public static void main(String[] args) throws Exception {
        if (args.length != 3) {
            throw new IllegalArgumentException(
                "Usage: JackcessIndexMergeCheck <database.mdb> <table> <index>");
        }
        try (Database database = new DatabaseBuilder().withPath(Paths.get(args[0])).open()) {
            Table table = database.getTable(args[1]);
            if (table == null) {
                throw new AssertionError("Jackcess did not open table " + args[1]);
            }
            if (table.getRowCount() != 300) {
                throw new AssertionError("Jackcess saw " + table.getRowCount() + " rows");
            }
            Cursor cursor = new CursorBuilder(table).withIndexByName(args[2]).toCursor();
            int expected = 401;
            Row row;
            while ((row = cursor.getNextRow()) != null) {
                int id = ((Number) row.get("Id")).intValue();
                if (id != expected) {
                    throw new AssertionError("Jackcess read Id " + id + " instead of " + expected);
                }
                ++expected;
            }
            if (expected != 701) {
                throw new AssertionError("Jackcess walked " + (expected - 401) + " entries");
            }

            // The merged tree must still accept inserts through Jackcess.
            table.addRow(900);
            if (table.getRowCount() != 301) {
                throw new AssertionError("Jackcess could not append after the merge");
            }
            Cursor after = new CursorBuilder(table).withIndexByName(args[2]).toCursor();
            int last = -1;
            while ((row = after.getNextRow()) != null) {
                last = ((Number) row.get("Id")).intValue();
            }
            if (last != 900) {
                throw new AssertionError("appended row is not the last index entry: " + last);
            }
        }

        System.out.println("Jackcess 5.2.0 read the merged index and appended a row");
    }
}
