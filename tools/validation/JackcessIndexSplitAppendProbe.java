import io.github.spannm.jackcess.Cursor;
import io.github.spannm.jackcess.CursorBuilder;
import io.github.spannm.jackcess.Database;
import io.github.spannm.jackcess.DatabaseBuilder;
import io.github.spannm.jackcess.Row;
import io.github.spannm.jackcess.Table;

import java.nio.file.Paths;

/**
 * Adds rows with Jackcess 5.2.0 to a C++ index tree that already has a node root and several
 * leaves. The new keys fall into the first, a middle and the last leaf, so Jackcess has to use the
 * node keys written by C++ to pick the correct child page.
 */
public final class JackcessIndexSplitAppendProbe {
    private JackcessIndexSplitAppendProbe() {}

    public static void main(String[] args) throws Exception {
        if (args.length != 2) {
            throw new IllegalArgumentException(
                "Usage: JackcessIndexSplitAppendProbe <database.mdb> <table>");
        }

        try (Database database = new DatabaseBuilder().withPath(Paths.get(args[0])).open()) {
            Table table = database.getTable(args[1]);
            if (table == null) {
                throw new AssertionError("Jackcess did not open table " + args[1]);
            }
            table.addRow(0);
            table.addRow(701);
            table.addRow(350);
            if (table.getRowCount() != 703) {
                throw new AssertionError("Jackcess observed " + table.getRowCount() + " rows");
            }
            java.util.List<Integer> expected = new java.util.ArrayList<>();
            for (int id = 0; id <= 701; ++id) {
                expected.add(id);
            }
            expected.add(350);
            java.util.Collections.sort(expected);

            Cursor cursor = new CursorBuilder(table).withIndexByName("IX_Id").toCursor();
            int position = 0;
            Row row;
            while ((row = cursor.getNextRow()) != null) {
                int id = ((Number) row.get("Id")).intValue();
                if (position >= expected.size() || id != expected.get(position)) {
                    throw new AssertionError("Jackcess read Id " + id + " at position " + position);
                }
                ++position;
            }
            if (position != expected.size()) {
                throw new AssertionError("Jackcess walked " + position + " entries");
            }
        }

        System.out.println("Jackcess 5.2.0 appended into the C++ split index tree and kept key order");
    }
}
