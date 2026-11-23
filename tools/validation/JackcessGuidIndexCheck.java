import io.github.spannm.jackcess.Cursor;
import io.github.spannm.jackcess.CursorBuilder;
import io.github.spannm.jackcess.Database;
import io.github.spannm.jackcess.DatabaseBuilder;
import io.github.spannm.jackcess.Row;
import io.github.spannm.jackcess.Table;

import java.nio.file.Paths;

/**
 * Verifies that Jackcess 5.2.0 reads ascending GUID index keys written by C++.
 *
 * Jet4 stores the first three UUID fields little-endian, so the index order is the stored byte
 * order: 0000…0001, then 0000…00ff, then 0100…0000.
 */
public final class JackcessGuidIndexCheck {
    private JackcessGuidIndexCheck() {}

    public static void main(String[] args) throws Exception {
        if (args.length != 3) {
            throw new IllegalArgumentException(
                "Usage: JackcessGuidIndexCheck <database.mdb> <table> <index>");
        }
        String[] expected = {
            "00000000-0000-0000-0000-000000000001",
            "00000000-0000-0000-0000-0000000000ff",
            "01000000-0000-0000-0000-000000000000",
        };
        try (Database database = new DatabaseBuilder().withPath(Paths.get(args[0])).withReadOnly(true).open()) {
            Table table = database.getTable(args[1]);
            if (table == null || table.getRowCount() != expected.length) {
                throw new AssertionError("Jackcess did not find the expected rows");
            }
            Cursor cursor = new CursorBuilder(table).withIndexByName(args[2]).toCursor();
            for (String uuid : expected) {
                Row row = cursor.getNextRow();
                if (row == null) {
                    throw new AssertionError("Jackcess read too few GUID index entries");
                }
                // Jackcess may hand back a UUID or its braced text form depending on the codec.
                String actual = row.get("Uuid").toString().toLowerCase()
                    .replace("{", "").replace("}", "");
                if (!actual.equals(uuid)) {
                    throw new AssertionError("Jackcess read " + actual + " instead of " + uuid);
                }
            }
            if (cursor.getNextRow() != null) {
                throw new AssertionError("Jackcess observed an unexpected extra GUID entry");
            }
        }

        System.out.println("Jackcess 5.2.0 read the GUID index entries in stored byte order");
    }
}
