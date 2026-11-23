import io.github.spannm.jackcess.Cursor;
import io.github.spannm.jackcess.CursorBuilder;
import io.github.spannm.jackcess.Database;
import io.github.spannm.jackcess.DatabaseBuilder;
import io.github.spannm.jackcess.Row;
import io.github.spannm.jackcess.Table;

import java.nio.file.Paths;

/** Verifies that Jackcess 5.2.0 reads C++-maintained ascending and descending MEMO indexes. */
public final class JackcessMemoIndexCheck {
    private JackcessMemoIndexCheck() {}

    public static void main(String[] args) throws Exception {
        if (args.length != 1) {
            throw new IllegalArgumentException("Usage: JackcessMemoIndexCheck <database.mdb>");
        }

        String longA = "x".repeat(700) + "A";
        String longB = "y".repeat(700) + "B";
        String[][] expected = {
            {"IX_Note", "Alpha", "Delta", longA, longB},
            {"IX_NoteDesc", longB, longA, "Delta", "Alpha"},
        };

        try (Database database = new DatabaseBuilder().withPath(Paths.get(args[0])).withReadOnly(true).open()) {
            Table table = database.getTable("MemoIndexed");
            if (table == null || table.getRowCount() != 4) {
                throw new AssertionError("Jackcess did not find the expected four-row MEMO table");
            }

            for (String[] index : expected) {
                Cursor cursor = new CursorBuilder(table).withIndexByName(index[0]).toCursor();
                for (int i = 1; i < index.length; ++i) {
                    Row row = cursor.getNextRow();
                    if (row == null || !index[i].equals(row.get("Note"))) {
                        throw new AssertionError("Jackcess read the wrong MEMO value from " + index[0]);
                    }
                }
                if (cursor.getNextRow() != null) {
                    throw new AssertionError("Jackcess observed an extra entry in " + index[0]);
                }
            }
        }

        System.out.println("Jackcess 5.2.0 read the maintained ascending and descending MEMO indexes");
    }
}
