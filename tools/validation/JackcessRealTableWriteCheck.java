import io.github.spannm.jackcess.Cursor;
import io.github.spannm.jackcess.CursorBuilder;
import io.github.spannm.jackcess.Database;
import io.github.spannm.jackcess.DatabaseBuilder;
import io.github.spannm.jackcess.Row;
import io.github.spannm.jackcess.Table;

import java.nio.file.Paths;

/**
 * Verifies that Jackcess 5.2.0 reads a real ArcGIS/Access table after C++ inserted and deleted a
 * row through the table's existing index.
 *
 * Usage: JackcessRealTableWriteCheck <database.mdb> <expected row count> <expected SRID order>
 */
public final class JackcessRealTableWriteCheck {
    private JackcessRealTableWriteCheck() {}

    public static void main(String[] args) throws Exception {
        if (args.length != 3) {
            throw new IllegalArgumentException(
                "Usage: JackcessRealTableWriteCheck <database.mdb> <rows> <srid,order,...>");
        }
        int expectedRows = Integer.parseInt(args[1]);
        String[] expectedOrder = args[2].split(",");

        try (Database database = new DatabaseBuilder().withPath(Paths.get(args[0])).withReadOnly(true).open()) {
            Table table = database.getTable("GDB_SpatialRefs");
            if (table == null) {
                throw new AssertionError("Jackcess did not open GDB_SpatialRefs");
            }
            if (table.getRowCount() != expectedRows) {
                throw new AssertionError("Jackcess saw " + table.getRowCount() + " rows");
            }
            Cursor cursor = new CursorBuilder(table).withIndexByName("SRID_Index").toCursor();
            int position = 0;
            Row row;
            while ((row = cursor.getNextRow()) != null) {
                int srid = ((Number) row.get("SRID")).intValue();
                if (position >= expectedOrder.length
                        || srid != Integer.parseInt(expectedOrder[position])) {
                    throw new AssertionError("Jackcess read SRID " + srid + " at position " + position);
                }
                if (srid == 4242) {
                    Object text = row.get("SRTEXT");
                    if (text == null || !text.toString().equals("GEOGCS[\"test\"]")) {
                        throw new AssertionError("Jackcess read the memo as " + text);
                    }
                }
                ++position;
            }
            if (position != expectedOrder.length) {
                throw new AssertionError("Jackcess walked " + position + " index entries");
            }
        }

        System.out.println("Jackcess 5.2.0 read " + expectedRows
            + " rows of a real Access table with the expected index order");
    }
}
