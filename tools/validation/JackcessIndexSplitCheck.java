import io.github.spannm.jackcess.Cursor;
import io.github.spannm.jackcess.CursorBuilder;
import io.github.spannm.jackcess.Database;
import io.github.spannm.jackcess.DatabaseBuilder;
import io.github.spannm.jackcess.Row;
import io.github.spannm.jackcess.Table;

import java.nio.file.Paths;

/**
 * Verifies that Jackcess 5.2.0 walks an index tree that C++ split into several leaf pages.
 *
 * The fixture inserts 700 rows with Id 1..700, which forces a leaf split and turns the index root
 * into a node page. Jackcess must descend the node, follow the leaf chain and return every key in
 * ascending order.
 */
public final class JackcessIndexSplitCheck {
    private JackcessIndexSplitCheck() {}

    public static void main(String[] args) throws Exception {
        if (args.length != 3) {
            throw new IllegalArgumentException(
                "Usage: JackcessIndexSplitCheck <database.mdb> <table> <index>");
        }
        int expectedRows = 700;

        try (Database database = new DatabaseBuilder().withPath(Paths.get(args[0])).withReadOnly(true).open()) {
            Table table = database.getTable(args[1]);
            if (table == null || table.getRowCount() != expectedRows) {
                throw new AssertionError("Jackcess did not find the expected row count");
            }
            Cursor cursor = new CursorBuilder(table).withIndexByName(args[2]).toCursor();
            int expected = 1;
            int seen = 0;
            Row row;
            while ((row = cursor.getNextRow()) != null) {
                int id = ((Number) row.get("Id")).intValue();
                if (id != expected) {
                    throw new AssertionError("Jackcess read Id " + id + " instead of " + expected);
                }
                ++expected;
                ++seen;
            }
            if (seen != expectedRows) {
                throw new AssertionError("Jackcess read " + seen + " index entries");
            }
        }

        System.out.println("Jackcess 5.2.0 walked the split index tree and read 700 ascending keys");
    }
}
